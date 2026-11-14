# LAEY extension packs: one CD layer, several masters

[中文](PACKS.zh.md)

An LAEY file holds one CD layer (the core layer in the 48 kHz family) and one or more **extension packs**. Each pack is computed as a whole from one master against that CD layer; the packs share nothing. Older players and decoders play the CD layer with the first pack (the main body) and are not affected. Normative text: [spec/lossy/SPEC.md](../spec/lossy/SPEC.md), Section 5.6.

## 1. Encoding

Encoding takes two steps, as with an independent CD (README Section 4.3): you confirm the positions.

### Step 1: the encoder searches the position of every further master

```
lae_enc master1.wav out.wav 3 s1 master2=master2.wav master3=master3.wav
```

For each further master, the encoder runs the whitened and the plain search, prints the suggested position (a line `ALIGN_SUGGEST …`) and stops without writing anything.

### Step 2: give the positions and encode

```
lae_enc master1.wav out.wav 3 s1 master1name="original master" master2=master2.wav master2align=+0.8459282 master2name="valve amplifier master" master3=master3.wav master3align=0 master3name="analog tape master"
```

| Option | Meaning |
|---|---|
| `master2=` … `master9=` | Further masters, numbered without gaps. Each gives one extension pack: `master2` is pack 1, `master3` is pack 2, and so on. |
| `master<n>align=<offset>` | The position of the master on the time line of the CD layer, written like `cdalign=` (in its own samples; fractions are allowed). |
| `master<n>name=<name>` | The name that a player shows for this pack; at most 255 bytes. Without it, the pack has no name. |
| `master1name=<name>` | The name that a player shows for the main body (the first master, pack 0); at most 255 bytes. Without it, the output is unchanged. |
| `master<n>accept` | Like `cdaccept`: continue when the remaining time difference lies beyond −40 dB, or after a warning about drift or check blocks that do not match. |
| `master<n>edge=pad` or `cut` | What to do when this master has sound outside the CD layer, like `cdedge=`. Without a choice, nothing is encoded. |

Where the CD layer comes from:

- Without `cd=`, the CD layer is derived from master 1.
- With `cd=your_cd.wav`, the CD layer is that CD; master 1 is aligned to it first (`cdalign=` and the other options, as before).

The mode, the scheme (`s1` / `s2`), `zs` and the other coding options are the same for all packs.

### Good to know

- The masters may have different rates within one family (44.1 kHz or 48 kHz).
- The samples of the CD layer are never changed. Where alignment needs it (a master moved by a fraction of a sample, or `…edge=pad`), digital silence is added at the ends of the CD layer: the largest need of all masters is taken and added once, and the same silence is added to every master.
- Every further master costs one more alignment and one more complete encode.
- Size: each pack adds only its own extension. The more a master differs from the CD layer, the larger its pack. On 10-second excerpts of the demonstration pieces, the CD layer took about 1.76 MB and each pack 0.6–1.1 MB.
- If the speed of a master drifts against the CD layer (for example, tape wow and flutter), or some of its check blocks do not match the CD layer, the encoder warns; it continues only with a position given by hand and a confirmation (`master<n>accept`, or the answer at the console), never on its own search.
- `g` can be used with further masters (each pack on its own master). Further masters cannot be combined with `noise=`, `resume` or `cdcheck`, and the whole-disc planner `lae_enc plan` cannot add further masters directly.

## 2. Decoding

```
lae_dec in.wav out.wav [full|16|cd24] [output rate] [pack=<number>]
```

- Without `pack=`, or with `pack=0`: the CD layer and the first pack, exactly as before.
- `pack=1`, `pack=2`, …: the CD layer and that pack.
- When the file has further packs, `lae_dec` lists each with its rate and name (and the name of the main body, if one is stored).
- The output rate and the 24-bit, 16-bit and cd24 routes work as before within the chosen pack.
- `lae_udec info <file>` lists every pack with its routes as choices (`pack1_full_176400`, …). `lae_udec dec --pick=<rate>:<bits>` chooses among the choices of the main body only; another pack is chosen with `--pick=pack:<k>` or by the name of one of its choices.

## 3. Other forms

- FLAC form (an output name ending in `.flac`): carries all packs.
- CD-Extra packaging (`lae_cdx`): the extension files carry all packs unchanged; the space needed grows with the packs.

## 4. Support

| Component | Support |
|---|---|
| `lae_enc`, `lae_dec` (x64, x86, Linux x64 and ARM64) | encoding and decoding of every pack |
| `liblae` (API 8 and later: `lae_pack_count`, `lae_pack_info`, `lae_select_pack`) and `lae_udec` | listing and selecting packs |
| foobar2000, VLC, Kodi and DirectShow plug-ins | selecting packs; foobar2000 lists them by their stored names, with their layers (the plug-in READMEs) |
| Java library | encoder, decoder and the same API, byte-identical with the C++ tools |
