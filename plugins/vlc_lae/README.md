# liblae_plugin.dll: LAE input module for VLC

**English** | [中文](README.zh.md)

This is an input (demux) module for VLC media player 3.0 (Windows, 64-bit). It opens LAE files: **LAE Lossless (LAES)** files (in WAV, RF64 or FLAC form) with their lossless masters, and **LAE Lossy (LAEY)** files (container version 5, WAV or FLAC form). VLC queries every input module about each file that it opens. This module accepts LAE files with **any extension** (`.lae`, `.flac`, `.wav`), so the masters play without the files being renamed. It rejects other files after reading only their structure, which takes milliseconds, and VLC's own modules continue to play plain FLAC and WAV files. The module delivers 2-channel PCM, with 16-bit samples or with 24-bit or 32-bit samples in 32-bit words, at the sample rate of what is played, and it supports seeking.

## Changes in this update

- LAEY files in the FLAC form are opened as well ([spec/lossy/SPEC.md](../../spec/lossy/SPEC.md), Section 3.4); files are recognised by content.
- LAEY extension packs (Section 5.6 of the same specification): the option "LAEY extension pack" (`lae-pack`) selects the pack. It counts from 0, as `lae_dec pack=` does: 0 = the main body, the pack of master 1; n = the pack of master n + 1 (foobar2000 and Kodi count from 1 in their menus, so their "Pack n + 1" is n here; the log names a pack in the same way as they do, with the lae-pack value: "Pack 3 (lae-pack 2) cannot be used; Pack 1 plays", Pack 1 being the main body). A file without that pack plays the main body; so does a file whose pack cannot be used, and only then is there a message in the log. No pack is chosen automatically.
- The option "LAEY layers" (`lae-layers`) offers "All layers", "Frequency layer only", "Bit-depth layer only" and "CD/core layer only".
- LAES payload version 2 ([spec/lossless/SPEC.md](../../spec/lossless/SPEC.md), Section 10.3): when nothing else is set, the default master stored in the file plays; without one, the core master.
- A master plays only after it and its chain of tiers have matched their SHA-256 values; the result is kept per file content. A derived 16-bit version starts at the seek position, bit for bit.
- Two standalone builds, English (`-en`) and Chinese (`-zh`). VLC 3.0 shows module options in English only.

## Installation (current user, no administrator rights)

Run `lae_vlc_setup-<version>.exe`. The installer:

- places the module in `%LOCALAPPDATA%\Programs\LAE\VLC`;
- adds that folder to your environment variable `VLC_PLUGIN_PATH` (VLC 3 also loads modules from the folders listed in this variable; existing entries are kept);
- adds an entry to the Windows list of installed apps, from which the module can be uninstalled.

Restart VLC afterwards.

Alternatively, install the module manually: copy `liblae_plugin.dll` into VLC's `plugins\demux` folder. For an installed copy of VLC, this requires administrator rights.

## What plays

The settings are in VLC > Tools > Preferences > Show settings: All > Input / Codecs > Demuxers > LAE. If no setting is made, an LAEY file plays all layers at the master rate, and an LAES file plays its default master (the master the publisher marked when encoding; a file without one, or whose payload failed its check, plays the core master). No master is chosen by its sample rate or bit depth ([spec/lossless/SPEC.md](../../spec/lossless/SPEC.md), Section 10.3); the other LAE players follow the same rule. The limits work inside that master only: its highest rate level up to the maximum sample rate plays (a rate level is the master brought down to a lower rate), and a word length above the maximum bit depth plays that entry's own derived 16-bit version; when no level of the default master fits, the core master plays. For LAES, "Bit-depth layer only" plays the default master's level at the base rate, and "CD/core layer only" plays the core master.

| Setting | Meaning |
|---|---|
| LAEY layers (`--lae-layers`) | 0 All layers (LAES: the master); 1 Bit-depth layer only (at the base rate); 2 CD/core layer only; 3 Frequency layer only (LAES: as 0) |
| LAEY extension pack (`--lae-pack`) | 0 = the main body; n = the pack of master n + 1 (see above) |
| Maximum sample rate (`--lae-maxrate`) | No limit, 192 kHz, 96 kHz or 48 kHz |
| LAES maximum bit depth (`--lae-maxbits`) | No limit, 16-bit, 24-bit or 32-bit |

## Rules (as in the other LAE players)

- A master is played as that master only after it has been decoded completely and has matched its SHA-256 hash. The first time a given file content is opened, all masters are checked in a single pass, and a progress dialog with a Cancel button is shown. If a master fails the check, or the check is cancelled, the core master plays and a VLC error dialog says so; after Cancel nothing is recorded, and the check runs again the next time the file is opened. The result is stored in the file `laes_verified.txt` in `%APPDATA%\LAE`, keyed by the SHA-256 hash of the whole file (each open computes this hash first, which takes a few seconds for a large file). The folder `%APPDATA%\LAE` is shared with the foobar2000, Kodi and DirectShow players, so a file that has been checked in any of them plays immediately in VLC.
- Seeking starts at the block that contains the target time. Block positions (for every 16th block; tens of kB) are stored in `%APPDATA%\LAE\laes_index\`, never in the media file.

## Tested (Windows 11, VLC 3.0.24 64-bit, 2026-11-04)

The module's output was written unchanged to a WAV file with VLC's stream output (`--sout "#std{access=file,mux=wav,...}"`) and compared sample by sample with the output of `laes dec` / `lae_dec`:

- A 15-second LAES file (`.lae`; master 352.8 kHz / 24 bit): identical. With the sample rate limited to 96 kHz, the 88.2 kHz rate level of the master played: identical.
- Grand (a demonstration piece of the LAE releases; 10:09; FLAC form, still named `.flac`; 4 masters): the first open checked all 4 masters, after which master 1 (352.8 kHz / 32 bit) played: identical. Playback started at 300 s: identical from that sample onwards.
- An LAEY file (`.wav`): identical to the output of `lae_dec` (all layers, 352.8 kHz).
- A plain WAV file: not accepted by the module; VLC's own WAV module played it.
- VLC's stream output drops the last 40-50 ms of every file, including when it uses its own WAV module. The comparison covers everything up to that point.
- During normal playback, VLC runs its own resampler to keep the sound card in time, so the signal that reaches the sound card is not bit-exact. VLC behaves in this way for every format; the samples that the module delivers are exact.

## Building

```bat
plugins\vlc_lae\build_vlc.cmd <VLC sdk folder> <output folder>
plugins\vlc_lae\build_setup.cmd <VLC sdk folder> <output folder>
```

The build requires VS2022 Build Tools (x64). The `sdk` folder (headers and `libvlccore.lib`) is included in VLC's 64-bit 7z package (`vlc-3.0.x-win64.7z`), but not in the zip package. The LAE decoders are compiled with their own verified compiler flags (`/fp:precise` for `lae_api`, `/fp:strict` for `laes_api`). Each build of the module targets a single VLC series (3.0); VLC 4 will require a new build.

## Licence

The source files are licensed under Apache-2.0. The module binary contains code from VLC's plug-in headers (LGPL-2.1-or-later) and, as a whole, is distributed under LGPL-3.0-or-later. The full licence text is in [LICENSES/LGPL-3.0-or-later.txt](../../LICENSES/LGPL-3.0-or-later.txt); it incorporates the terms of the GNU GPL version 3, whose text is in [LICENSES/GPL-3.0-or-later.txt](../../LICENSES/GPL-3.0-or-later.txt). The installer program (`setup/setup.cpp`) is licensed under Apache-2.0; the module that it installs keeps the licence given above.
