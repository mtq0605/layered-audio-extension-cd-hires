# lae_source.ax: LAE source filter for DirectShow

**English** | [中文](README.zh.md)

A DirectShow source filter for PotPlayer, MPC-BE, MPC-HC, Windows Media Player (legacy) and other DirectShow players (Windows x64).
It opens LAE files: **LAES** (LAE Lossless; WAV, RF64 or FLAC form) with its lossless masters, and **LAEY** (LAE v5 WAV). It delivers
uncompressed PCM (2 channels; 16-bit, or 24-bit and 32-bit samples in 32-bit containers as WAVE_FORMAT_EXTENSIBLE; 24-bit packed in 3
bytes was turned into noise by MPC-BE's audio path, so it is not used) at the rate of what plays, with seeking. Written for this project
from the DirectShow interface documentation (no base classes, no third-party code).

## Installation (current user, no administrator rights)

Run `lae_dshow_setup-<version>.exe`: it puts the filter in `%LOCALAPPDATA%\Programs\LAE\DirectShow`, registers it and adds an entry to
Windows' installed apps list (remove it there). Or by hand:

```bat
regsvr32 lae_source.ax
```

This registers the filter (the COM class, the DirectShow filter catalogue entry and the source for the extension `.lae`); without
administrator rights it goes to the current user only (`HKCU\Software\Classes`). A `.flac` / `.wav` LAE file keeps
playing its core master (can be CD-compatible) in every player; to play its masters, either rename it to `.lae`, or add the filter in the player:

- PotPlayer (64-bit): Preferences (F5) → Filters → Global filter priority → Add external filter → `lae_source.ax` → Force use (tested).
- MPC-BE / MPC-HC (64-bit): Options → External Filters → Add Filter → LAE source → Prefer.

Remove: `regsvr32 /u lae_source.ax`.

## What plays

Registry values (DWORD) under `HKCU\Software\LAE\DirectShow` (none set = the master at its own rate):

| Value | Meaning |
|---|---|
| `layers` | 0 all layers / the master; 1 CD layer + depth layer (base rate); 2 CD layer only |
| `maxrate` | highest sample rate in Hz, 0 = no limit (for example 96000 for a sound card or renderer that cannot take more) |
| `maxbits` | LAES: highest word length, 0 = no limit, 16, 24 or 32 |

## Rules (as in the foobar2000 component)

- A master plays as that master only after it has been decoded completely and matched its SHA-256. The first open of a file content checks
  all masters in one pass with a small progress window (Grand, 10 min, 4 masters: about 1 minute on the test machine); the result is kept by
  the SHA-256 of the whole file in `%APPDATA%\LAE\laes_verified.txt`, so later opens take a few seconds (the file check).
- Seeking starts at the block that holds the time; block positions are kept in `%APPDATA%\LAE\laes_index\` (every 16th block,
  tens of kB; never written into the media file).

## Tested (Windows 11, DirectShow graph with the standard audio renderer)

- A 15-second LAES file and Grand (10:09, 4 masters, FLAC form): load, play, seek to 60 % at once; the first open of Grand checked all
  masters (63 s), the second opened in 3 s.
- PotPlayer 64-bit (2026-11-03): the filter added as an external filter (Force use); a 15-second LAES file played its master at 352.8 kHz / 24 bit
  (PCM 16934 kb/s shown by PotPlayer); seeking while playing worked. The standard Windows audio renderer does not take 352.8 kHz (maxrate helps there).
- MPC-BE 1.9.2 x64 (portable, 2026-11-03), external filter preferred: with 24-bit packed in 3 bytes its audio path played noise (a loopback
  recording of the sound card showed ~15000 zero crossings per second instead of ~750); with 24-bit in 32-bit containers it plays the
  music (recording: ~700-830 crossings per second, as PotPlayer). The delivered bytes equal the decoded master (x 256 for 24-bit).

## Building

```bat
plugins\dshow_lae\build_dshow.cmd <output folder>
```

VS2022 Build Tools, x64; the Windows SDK's DirectShow headers and `strmiids.lib`. The LAE decoders are compiled with their own verified
flags (liblae `/fp:precise`, the LAES decoder `/fp:strict`).

## Licence

Apache-2.0.
