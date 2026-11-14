# audiodecoder.lae: LAE decoder for Kodi

**English** | [中文](README.zh.md)

This add-on plays LAE files in Kodi 21 (Omega). It supports **LAES** (LAE Lossless) files in WAV, RF64 or FLAC form, including their lossless masters, and **LAEY** (LAE Lossy) files of container version 5 in WAV or FLAC form.

## Changes in this update

- LAEY files in the FLAC form are decoded as well ([spec/lossy/SPEC.md](../../spec/lossy/SPEC.md), Section 3.4). As before, Kodi passes only files with the extension `.lae` to the add-on.
- LAEY extension packs (Section 5.6 of the same specification): the setting "LAEY extension pack" (Pack 1 … Pack 9) selects the pack. "Pack n" is the pack of master n, counted from 1 as in the foobar2000 menu; `lae_dec pack=`, DirectShow and VLC count from 0 and call it pack n − 1 ("Pack 1" is the main body; the log uses the same names as the setting: "Pack 3 cannot be used; Pack 1 plays"). A file without that pack plays pack 1. No pack is chosen automatically.
- The setting "LAEY layers" offers "All layers", "Frequency layer only", "Bit-depth layer only" and "CD/core layer only".
- LAES payload version 2 ([spec/lossless/SPEC.md](../../spec/lossless/SPEC.md), Section 10.3): when nothing else is set, the default master stored in the file plays; without one, the core master.
- A master plays only after it and its chain of tiers have matched their SHA-256 values; the result is kept per file content. A derived 16-bit version starts at the seek position, bit for bit.
- Two packages, English (`-en`) and Chinese (`-zh`), each with one language (`lang_pkg.ps1`, called by `build_kodi.cmd`).

## Files: the extension `.lae`

Kodi passes to a decoder add-on only files with the extensions that the add-on declares. It does not allow the add-on to return a file to Kodi's built-in decoder. An add-on that declared `.flac` or `.wav` would therefore have to decode every FLAC and WAV file itself. For this reason, this add-on declares only the format's own extension, **`.lae`**:

- To play the masters of an LAE file (`.flac` or `.wav`, including the RF64 form) in Kodi, rename the file to `.lae`.
- With the extension `.flac` or `.wav`, the file is played by Kodi's built-in decoder, which plays the core master (the CD layer for LAEY). The core master can be CD-compatible (this is how LAE is intended to work in any player).

## Installation

In Kodi, select Add-ons → Install from zip file → `audiodecoder.lae-<version>.zip`. Kodi asks once whether to allow unknown sources. Platform: Windows x64.

## What plays

The settings are under Add-ons → My add-ons → Audio decoder → LAE decoder → Configure:

| Setting | Values |
|---|---|
| LAEY layers | All layers (LAES: the master); Bit-depth layer only (CD or core layer + depth layer, base rate); CD/core layer only (LAES: the core master); Frequency layer only (LAES: as All layers) |
| Maximum sample rate | No limit; 192 kHz; 96 kHz |
| LAES maximum bit depth | No limit; 16-bit; 24-bit; 32-bit |
| LAEY extension pack | Pack 1 … Pack 9 (see above) |

For LAES, the file's default master plays (the master the publisher marked when encoding; a file without one, or whose payload failed its check, plays the core master); no master is chosen by its sample rate or bit depth ([spec/lossless/SPEC.md](../../spec/lossless/SPEC.md), Section 10.3). The limits work inside that master only: its highest rate level up to "Maximum sample rate" plays (a rate level is a master brought down to half, a quarter or an eighth of its rate), and a word length above "LAES maximum bit depth" plays that entry's own derived 16-bit version; when no level of the default master fits, the core master plays. This is the same rule as in the foobar2000 component.

## Rules (as in the foobar2000 component)

- A master is played as that master only after it has been decoded completely and has matched the SHA-256 stored in the file. The first time a file's content is played, all its masters are checked in a single pass, with a progress window (Grand, a demonstration piece of the LAE releases: 10 min, 4 masters, about 2 minutes on the test machine). The result is stored under the SHA-256 of the whole file in `laes_verified.txt` (on Windows in `%APPDATA%\LAE`, shared with the other LAE players; on other platforms in the add-on's data folder), so that later plays start immediately. A master that fails the check is not played; the core master is played instead.
- Seeking starts at the block that contains the target time. The block positions are stored in `laes_index\<SHA-256>.idx` (on Windows in `%APPDATA%\LAE`; on other platforms in the add-on's data folder) (every 16th block; tens of kB; never written into the media file).
- Output: 16-bit, 24-bit (in 32-bit containers) or 32-bit integer PCM, at the sample rate of the selected master or layer, without modification.

## Tested (Kodi 21.3 x64, portable copy)

- A 15-second LAES file and Grand (10:09, 4 masters, FLAC form), each with the extension `.lae`: on the first play, the masters were checked and then the master played (352.8 kHz / 32 bit); seeking to about 60 % worked immediately (Kodi showed 6:07); the second play started without a check (about 0.8 s after opening).
- Not tested here: other platforms (Linux, macOS and Android require their own builds), LAEY files and network paths.

## Building

```bat
plugins\kodi_audiodecoder_lae\build_kodi.cmd <kodi-dev-kit include folder> <output folder>
```

The kodi-dev-kit headers are taken from the Kodi repository (`xbmc/addons/kodi-dev-kit/include`, tag `21.3-Omega`). Requirements: VS2022 Build Tools, x64. The LAE decoders are compiled with their own flags, the ones verified for them in [docs/BUILD.md](../../docs/BUILD.md), Section 1 (liblae: `/fp:precise`; LAES decoder: `/fp:strict`).

## Licence

Source code of this add-on: Apache-2.0. The kodi-dev-kit headers compiled into the binary are licensed under GPL-2.0-or-later; the add-on binary as a whole is therefore distributed under GPL-3.0-or-later.
