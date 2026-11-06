# lae_source.ax: LAE source filter for DirectShow

**English** | [中文](README.zh.md)

`lae_source.ax` is a DirectShow source filter for PotPlayer, MPC-BE, MPC-HC, Windows Media Player (legacy) and other DirectShow players (Windows x64). It opens two kinds of LAE file:

- **LAES** (LAE Lossless; WAV, RF64 or FLAC form), with its lossless masters.
- **LAEY** (LAE Lossy; WAV, container version 5).

The filter delivers uncompressed PCM, with seeking, at the sample rate of what is played (see [What plays](#what-plays)):

- 2 channels.
- 16-bit samples, or 24-bit and 32-bit samples in 32-bit containers as WAVE_FORMAT_EXTENSIBLE.

24-bit samples packed in 3 bytes are not used because MPC-BE's audio path turned them into noise. The filter was written for this project from the DirectShow interface documentation, without base classes or third-party code.

## Installation (current user, no administrator rights)

Run `lae_dshow_setup-<version>.exe`. The installer puts the filter in `%LOCALAPPDATA%\Programs\LAE\DirectShow`, registers it and adds an entry to the list of installed apps in Windows. Remove the filter from that list. Alternatively, register the filter manually:

```bat
regsvr32 lae_source.ax
```

This registers the COM class, the entry in the DirectShow filter catalogue, and the filter as the source for the `.lae` extension. Without administrator rights, registration applies to the current user only (`HKCU\Software\Classes`).

An LAE file with the extension `.flac` or `.wav` continues to play its core master (which can be CD-compatible) in every player. To play its masters, either rename the file to `.lae` or add the filter in the player:

- PotPlayer (64-bit): Preferences (F5) → Filters → Global filter priority → Add external filter → `lae_source.ax` → Force use (tested).
- MPC-BE / MPC-HC (64-bit): Options → External Filters → Add Filter → LAE source → Prefer.

To unregister the filter, run `regsvr32 /u lae_source.ax`.

## What plays

Playback is controlled by DWORD registry values under `HKCU\Software\LAE\DirectShow`. If no value is set, the filter plays the master at its own sample rate.

| Value | Meaning |
|---|---|
| `layers` | 0: all layers / the master; 1: CD layer and depth layer (base rate); 2: CD layer only |
| `maxrate` | Highest sample rate in Hz; 0 = no limit (for example, 96000 for a sound card or renderer that cannot accept higher rates) |
| `maxbits` | LAES only: highest word length; 0 = no limit, 16, 24 or 32 |

## Rules (as in the foobar2000 component)

- A master is played as that master only after it has been decoded completely and its SHA-256 has matched. The first time a given file content is opened, all masters are checked in one pass, with a small progress window. For Grand (10 min, 4 masters), this took about 1 minute on the test machine. The result is stored by the SHA-256 of the whole file in `%APPDATA%\LAE\laes_verified.txt`, so later opens take a few seconds (the file check).
- Seeking starts at the block that contains the requested time. Block positions (every 16th block, tens of kB) are stored in `%APPDATA%\LAE\laes_index\` and are never written to the media file.

## Tested (Windows 11, DirectShow graph with the standard audio renderer)

- A 15-second LAES file and Grand (10:09, 4 masters, FLAC form): both loaded, played and sought to 60 % immediately. The first open of Grand checked all masters (63 s); the second open took 3 s.
- PotPlayer 64-bit (2026-11-03), with the filter added as an external filter (Force use): a 15-second LAES file played its master at 352.8 kHz / 24 bit (PotPlayer showed PCM 16934 kb/s), and seeking during playback worked. The standard Windows audio renderer does not accept 352.8 kHz; `maxrate` helps in that case.
- MPC-BE 1.9.2 x64 (portable, 2026-11-03), with the filter preferred as an external filter: with 24-bit samples packed in 3 bytes, its audio path played noise. A loopback recording of the sound card showed ~15000 zero crossings per second instead of ~750. With 24-bit samples in 32-bit containers, it plays the music, with ~700-830 crossings per second in the recording, as with PotPlayer. The delivered bytes equal the decoded master (x 256 for 24-bit).

## Building

```bat
plugins\dshow_lae\build_dshow.cmd <output folder>
```

Requirements: VS2022 Build Tools (x64), and the DirectShow headers and `strmiids.lib` from the Windows SDK. The LAE decoders are compiled with their own verified flags: liblae with `/fp:precise` and the LAES decoder with `/fp:strict`.

## Licence

Apache-2.0.
