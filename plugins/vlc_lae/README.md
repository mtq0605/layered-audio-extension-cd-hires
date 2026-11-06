# liblae_plugin.dll: LAE input module for VLC

**English** | [中文](README.zh.md)

This is an input (demux) module for VLC media player 3.0 (Windows, 64-bit). It opens LAE files: **LAE Lossless (LAES)** files (in WAV, RF64 or FLAC form) with their lossless masters, and **LAE Lossy (LAEY)** files (LAE v5 WAV). VLC queries every input module about each file that it opens. This module accepts LAE files with **any extension** (`.lae`, `.flac`, `.wav`), so the masters play without the files being renamed. It rejects other files after reading only their structure, which takes milliseconds, and VLC's own modules continue to play plain FLAC and WAV files. The module delivers 2-channel PCM, with 16-bit samples or with 24-bit or 32-bit samples in 32-bit words, at the sample rate of what is played, and it supports seeking.

## Installation (current user, no administrator rights)

Run `lae_vlc_setup-<version>.exe`. The installer:

- places the module in `%LOCALAPPDATA%\Programs\LAE\VLC`;
- adds that folder to your environment variable `VLC_PLUGIN_PATH` (VLC 3 also loads modules from the folders listed in this variable; existing entries are kept);
- adds an entry to the Windows list of installed apps, from which the module can be uninstalled.

Restart VLC afterwards.

Alternatively, install the module manually: copy `liblae_plugin.dll` into VLC's `plugins\demux` folder. For an installed copy of VLC, this requires administrator rights.

## What plays

The settings are in VLC > Tools > Preferences > Show settings: All > Input / Codecs > Demuxers > LAE. If no setting is made, the master plays at its own sample rate.

| Setting | Meaning |
|---|---|
| What plays (`--lae-layers`) | All layers / the master; CD layer + depth layer (at the base rate); CD layer only |
| Highest sample rate (`--lae-maxrate`) | No limit, 192 kHz, 96 kHz or 48 kHz |
| Highest word length (`--lae-maxbits`) | LAES: no limit, 16, 24 or 32 bit |

## Rules (as in the other LAE players)

- A master is played as that master only after it has been decoded completely and has matched its SHA-256 hash. The first time a given file content is opened, all masters are checked in a single pass, and a progress dialog is shown. The result is stored in `%APPDATA%\LAE\laes_verified.txt`, keyed by the SHA-256 hash of the whole file. This folder is shared with the foobar2000, Kodi and DirectShow players, so a file that has been checked in any of them plays immediately in VLC.
- Seeking starts at the block that contains the target time. Block positions (for every 16th block; tens of kB) are stored in `%APPDATA%\LAE\laes_index\`, never in the media file.

## Tested (Windows 11, VLC 3.0.24 64-bit, 2026-11-04)

The module's output was written unchanged to a WAV file with VLC's stream output (`--sout "#std{access=file,mux=wav,...}"`) and compared sample by sample with the output of `laes_tool dec` / `lae_dec`:

- A 15-second LAES file (`.lae`; master 352.8 kHz / 24 bit): identical. With the sample rate limited to 96 kHz, the 88.2 kHz level played: identical.
- Grand (10:09; FLAC form, still named `.flac`; 4 masters): the first open checked all 4 masters, after which master 1 (352.8 kHz / 32 bit) played: identical. Playback started at 300 s: identical from that sample onwards.
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

The source files are licensed under Apache-2.0. The module binary contains code from VLC's plug-in headers (LGPL-2.1-or-later) and, as a whole, is distributed under LGPL-3.0-or-later. The installer is licensed under Apache-2.0.
