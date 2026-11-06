# LAES at a glance

**English** | [中文](OVERVIEW.zh.md)

**LAES** (LAE Lossless, ID `LAE2`) puts a **core master** and further **lossless masters** into one ordinary FLAC or WAV file. A player without LAE software plays the core master. A player with LAE support also plays the other masters, after checking each against its SHA-256. All of it is lossless: every master comes back bit for bit.

- **The core master** is a lossless master in its own right: integer PCM at 44.1 or 48 kHz, 16, 24 or 32 bit. At 16 bit it can be CD-compatible. A core master at 24 or 32 bit, or one in the FLAC form, cannot be read by a CD player.
- **The other masters** run at 1, 2, 4 or 8 times the base rate (up to 352.8 / 384 kHz), at 16, 24 or 32 bit. Each one is stored as the difference to a signal already in the file (the core master or an earlier master), or in full, whichever is smaller.
- **Derived 16-bit masters** exist for every rate that has no 16-bit master of its own. They are lossless relative to their definition: every decoder reproduces them bit for bit. They are not lossless relative to the master they are made from: that master is requantised to 16 bits with TPDF dither, and it cannot be recovered from them. They are derived masters, not the original master.

Normative text: [SPEC.md](SPEC.md). Which software plays what without LAE: [COMPATIBILITY.md](../COMPATIBILITY.md).

## Compared with FLAC and WAV

Measured 2026-11-05 on two 10-second excerpts (2L 02 and 05). Each holds a core master and three masters: 352.8 kHz / 32 bit, 352.8 kHz / 24 bit and 176.4 kHz / 24 bit. The separate FLAC files were made with FFmpeg at compression level 8.

| Storing the same four versions as | Excerpt 02 | Excerpt 05 |
|---|---|---|
| one LAES file, FLAC form (24-bit core master) | **13.9 MB** | **15.2 MB** |
| four FLAC files (24-bit core master + three masters) | 24.8 MB | 28.7 MB |
| one LAES file, WAV form (16-bit core master) | 16.6 MB | 17.5 MB |
| four WAV files (16-bit core master + three masters) | 61.7 MB | 61.7 MB |
| for scale: the core master alone as FLAC | 1.1 MB | 1.5 MB |

These figures hold for masters made from the same source, as in this test. Masters with a different mix, level or edit have little in common, and the encoder then stores them more or less in full.

## Advantages

- **One file, every version.** A publisher hands out a single file. The listener chooses the version, or the player picks one within the limits the listener sets.
- **Smaller than separate files.** In the test above, LAES is 44 to 47 % smaller than the same versions as separate FLAC files, and its WAV form is 72 to 73 % smaller than the same versions as separate WAV files.
- **Plays without LAE.** The file is standard FLAC or WAV, so any complete FLAC or WAV decoder plays the core master. Tested: libFLAC, FFmpeg, Kodi, VLC, foobar2000 and Android's own decoder.
- **Lossless and checked.** Each master carries its SHA-256. A player uses a master only after it has decoded completely and matched.
- **Tags and pictures** sit before the masters in the FLAC form (Vorbis comments, PICTURE) and as RIFF INFO plus ID3v2.4 in the WAV form, all in UTF-8. Changing them (`laes tag`) does not touch the audio.
- **Tools around it:** `laes_export` writes any one version as plain FLAC for streaming servers; `laes_disc.ps1` makes a data-disc image of an album.

## Disadvantages and limits

- **The masters need LAE software.** Without it only the core master plays.
- **FLAC form: the masters come first.** FLAC requires all metadata before the audio frames. As a result:
  - Windows' built-in decoder (Media Foundation, Explorer) does not recognise the file;
  - a player that reads the file itself over the network must load the masters before the first sound;
  - there is no seek table, so seeking in some players is slower than in a plain FLAC.
- **WAV form: RF64 above 4 GiB.** Many WAV readers, Android's among them, do not open RF64.
- **A 32-bit core master** does not play in many decoders: Windows and Android do not decode 32-bit FLAC. The reference encoder therefore uses a core master of at most 24 bit unless asked otherwise.
- **Encoding takes time, memory and temporary disk.** 30 minutes with 12 masters took about 36 minutes, 1.4 GB of memory and about 28 GB of temporary space (2026-11-05).
- **The file is much larger than the core master alone,** because it carries every master.

## Which form to use

- **FLAC form** (`.flac`, or `.lae` with `--form=flac`): smallest; the core master may be 24 bit; tags as in any FLAC file. Not for Windows' own players.
- **WAV form** (`.wav`, or `.lae` with `--form=wav`): a 16-bit core master in the `data` chunk, which can be CD-compatible. Windows Explorer reads its length (tested); playback in Windows' own players was not tested. Larger, and RF64 beyond 4 GiB.
- **For streaming,** serve plain FLAC made with `laes_export`, not the LAES file itself.
