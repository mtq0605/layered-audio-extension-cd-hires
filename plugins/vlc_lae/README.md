# liblae_plugin.dll: LAE input module for VLC

**English** | [中文](README.zh.md)

An input (demux) module for VLC media player 3.0, Windows 64-bit. It opens LAE files: **LAES** (LAE Lossless; WAV, RF64 or FLAC form)
with its lossless masters, and **LAEY** (LAE v5 WAV). VLC asks every input module about each file it opens. This module takes LAE files
with **any extension** (`.lae`, `.flac`, `.wav`), so the masters play without renaming. It turns other files down after reading only their
structure (milliseconds), and VLC's own modules play plain FLAC and WAV as before. It delivers PCM: 2 channels, 16-bit samples, or 24-bit
and 32-bit samples in 32-bit words, at the rate of what plays, with seeking.

## Installation (current user, no administrator rights)

Run `lae_vlc_setup-<version>.exe`. It puts the module in `%LOCALAPPDATA%\Programs\LAE\VLC`, adds that folder to your environment variable
`VLC_PLUGIN_PATH` (VLC 3 also loads modules from the folders listed there; entries already there are kept) and adds an entry to
Windows' installed apps list (remove it there). Start VLC again afterwards.

Or by hand: copy `liblae_plugin.dll` into VLC's `plugins\demux` folder (for an installed VLC this needs administrator rights).

## What plays

VLC > Tools > Preferences > Show settings: All > Input / Codecs > Demuxers > LAE (none set = the master at its own rate):

| Setting | Meaning |
|---|---|
| What plays (`--lae-layers`) | all layers / the master; CD layer + depth layer (base rate); CD layer only |
| Highest sample rate (`--lae-maxrate`) | no limit, 192 kHz, 96 kHz or 48 kHz |
| Highest word length (`--lae-maxbits`) | LAES: no limit, 16, 24 or 32 bit |

## Rules (as in the other LAE players)

- A master plays as that master only after it has been decoded completely and matched its SHA-256. The first open of a file content checks
  all masters in one pass, with a progress dialog. The result is kept by the SHA-256 of the whole file in `%APPDATA%\LAE\laes_verified.txt`,
  the folder shared with the foobar2000, Kodi and DirectShow players, so a file checked in any of them plays at once here.
- Seeking starts at the block that holds the time. Block positions are kept in `%APPDATA%\LAE\laes_index\` (every 16th block, tens of
  kB), never in the media file.

## Tested (Windows 11, VLC 3.0.24 64-bit, 2026-11-04)

The module's output was written unchanged to a WAV with VLC's stream output (`--sout "#std{access=file,mux=wav,...}"`) and compared sample by
sample with `laes_tool dec` / `lae_dec`:

- A 15-second LAES file (`.lae`, master 352.8 kHz / 24 bit): identical. With the rate limit 96 kHz: the 88.2 kHz level, identical.
- Grand (10:09, FLAC form, still named `.flac`, 4 masters): the first open checked all 4 masters, then master 1 (352.8 kHz / 32 bit)
  played; identical. Started at 300 s: identical from that sample on.
- An LAEY file (`.wav`): identical to `lae_dec` (all layers, 352.8 kHz).
- A plain WAV: not taken; VLC's own WAV module played it.
- VLC's stream output drops the last 40-50 ms of every file, also with its own WAV module. The comparison covers everything up to there.
- VLC's normal playback runs its own resampler to keep the sound card in time, so what reaches the sound card is not bit-exact. This is
  VLC's behaviour for every format; the samples the module delivers are exact.

## Building

```bat
plugins\vlc_lae\build_vlc.cmd <VLC sdk folder> <output folder>
plugins\vlc_lae\build_setup.cmd <VLC sdk folder> <output folder>
```

VS2022 Build Tools, x64. The `sdk` folder (headers and `libvlccore.lib`) is in VLC's 64-bit 7z package (`vlc-3.0.x-win64.7z`; not in the
zip). The LAE decoders are compiled with their own verified flags (`lae_api` `/fp:precise`, `laes_api` `/fp:strict`). A module is built for
one VLC series (3.0); VLC 4 will need a new build.

## Licence

The source files are Apache-2.0. The module binary contains code from VLC's plugin headers (LGPL-2.1-or-later) and as a whole is
distributed under LGPL-3.0-or-later. The installer is Apache-2.0.
