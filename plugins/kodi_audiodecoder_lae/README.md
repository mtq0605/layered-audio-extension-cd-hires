# audiodecoder.lae: LAE decoder for Kodi

**English** | [中文](README.zh.md)

Plays LAE files in Kodi 21 (Omega): **LAES** (LAE Lossless; WAV, RF64 or FLAC form) with its lossless masters, and **LAEY** (LAE v5 WAV).

## Files: the extension `.lae`

Kodi gives a decoder add-on only the file extensions it declares, and does not let it hand a file back to Kodi's own decoder. An add-on
that took `.flac` or `.wav` would therefore have to decode every FLAC and WAV file itself. So this add-on takes only the format's own
extension, **`.lae`**:

- rename an LAE file (`.flac` or `.wav`) to `.lae` to play its masters in Kodi;
- as `.flac` / `.wav`, Kodi plays it with its own decoder: the core master, which can be CD-compatible (that is how LAE is meant to work in any player).

## Installation

Kodi → Add-ons → Install from zip file → `audiodecoder.lae-<version>.zip` (Kodi asks once to allow unknown sources). Windows x64.

## What plays

Settings (Add-ons → My add-ons → Audio decoder → LAE decoder → Configure):

| Setting | Values |
|---|---|
| What plays | All layers / the master; CD layer + depth layer (base rate); CD layer only |
| Highest sample rate | No limit; 192 kHz; 96 kHz |
| LAES: highest word length | No limit; 16; 24; 32 bit |

LAES: the highest rate within the limit, then the longest word; masters before rate levels (as in the foobar2000 component).

## Rules (as in the foobar2000 component)

- A master plays as that master only after it has been decoded completely and matched the SHA-256 stored in the file. The first play of a
  file content checks all its masters in one pass, with a progress window (Grand, 10 min, 4 masters: about 2 minutes on the test machine);
  the result is kept by the SHA-256 of the whole file (`laes_verified.txt`; on Windows in `%APPDATA%\LAE`, shared with the other LAE players, elsewhere in the add-on's data folder), so later plays start at once. A
  master that fails is not played; the core master plays instead.
- Seeking starts at the block that holds the time; the block positions are kept in `laes_index\<SHA-256>.idx` (every 16th block, tens of
  kB; never written into the media file).
- Output: 16-bit, 24-bit (in 32-bit containers) or 32-bit integer PCM at the choice's own rate, unchanged.

## Tested (Kodi 21.3 x64, portable copy)

- A 15-second LAES file and Grand (10:09, 4 masters, FLAC form) as `.lae`: the first play checked the masters, then the master played
  (352.8 kHz / 32 bit); seeking to 60 % reached 6:07 at once; the second play started without a check (about 0.8 s after opening).
- Not tested here: other platforms (Linux, macOS, Android need their own builds), LAEY files, network paths.

## Building

```bat
plugins\kodi_audiodecoder_lae\build_kodi.cmd <kodi-dev-kit include folder> <output folder>
```

The kodi-dev-kit headers come from Kodi's repository (`xbmc/addons/kodi-dev-kit/include`, tag `21.3-Omega`). VS2022 Build Tools, x64.
The LAE decoders are compiled with their own verified flags (liblae `/fp:precise`, the LAES decoder `/fp:strict`).

## Licence

This add-on's source: Apache-2.0. The kodi-dev-kit headers compiled into the binary are GPL-2.0-or-later, so the add-on binary as a whole
is distributed under GPL-3.0-or-later.
