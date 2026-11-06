# LAES at a glance

**English** | [中文](OVERVIEW.zh.md)

**LAES** (LAE Lossless, ID `LAE2`) stores a **core master** and further **lossless masters** in one ordinary FLAC or WAV file. A player without LAE software plays the core master. A player with LAE software can also play the other masters, after checking each one against its SHA-256. Every stored master is lossless and is reproduced bit for bit.

- **The core master** is a lossless master in its own right: integer PCM at 44.1 or 48 kHz, 16, 24 or 32 bit. At 16 bit, it can be CD-compatible. A CD player cannot read a core master at 24 or 32 bit, or a core master in the FLAC form.
- **The other masters** run at 1, 2, 4 or 8 times the base rate (up to 352.8 / 384 kHz), at 16, 24 or 32 bit. Each one is stored either as the difference from a signal already in the file (the core master or an earlier master) or in full, whichever is smaller.
- **Derived masters** (16 bit) exist for every rate that has no 16-bit master of its own. A derived master is lossless relative to its definition: every decoder reproduces it bit for bit. It is not lossless relative to the master it is derived from: that master is requantised to 16 bits with TPDF dither and cannot be recovered from the derived master. A derived master is not the original master.

Normative text: [SPEC.md](SPEC.md). Which software plays which form without LAE software: [COMPATIBILITY.md](../COMPATIBILITY.md).

## Compared with FLAC and WAV

Measured on 2026-11-05 using two 10-second excerpts (2L 02 and 05). For each excerpt, the file holds a core master and three further masters: 352.8 kHz / 32 bit, 352.8 kHz / 24 bit and 176.4 kHz / 24 bit. The separate FLAC files were made with FFmpeg at compression level 8.

| Storing the same four versions as | Excerpt 02 | Excerpt 05 |
|---|---|---|
| one LAES file, FLAC form (24-bit core master) | **13.9 MB** | **15.2 MB** |
| four FLAC files (24-bit core master + three masters) | 24.8 MB | 28.7 MB |
| one LAES file, WAV form (16-bit core master) | 16.6 MB | 17.5 MB |
| four WAV files (16-bit core master + three masters) | 61.7 MB | 61.7 MB |
| for scale: the core master alone as FLAC | 1.1 MB | 1.5 MB |

These figures apply to masters made from the same source, as in this test. Masters with a different mix, level or edit have little in common, and the encoder then stores them more or less in full.

## Advantages

- **One file, every version.** A publisher distributes a single file. The listener chooses the version, or the player chooses one within the limits that the listener sets.
- **Smaller than separate files.** In the test above, the LAES file is 44 to 47 % smaller than the same versions as separate FLAC files, and its WAV form is 72 to 73 % smaller than the same versions as separate WAV files.
- **Plays without LAE software.** The file is standard FLAC or WAV, so any complete FLAC or WAV decoder plays the core master, within the limits listed below (Windows' built-in FLAC decoder, RF64, 32-bit FLAC; [COMPATIBILITY.md](../COMPATIBILITY.md)). Tested: libFLAC, FFmpeg, Kodi, VLC, foobar2000 and Android's built-in decoder.
- **Lossless and verified.** Each master carries its SHA-256. A player uses a master only after the master has been decoded completely and its SHA-256 has matched.
- **Tags and pictures.** In the FLAC form, they are stored before the masters (Vorbis comments, PICTURE); in the WAV form, they are stored as RIFF INFO plus ID3v2.4. All are encoded in UTF-8. Changing them (`laes tag`) does not alter the audio.
- **Supporting tools.** `laes_export` writes any single version as plain FLAC for streaming servers; `laes_disc.ps1` creates a data-disc image of an album.

## Disadvantages and limits

- **The masters require LAE software.** Without it, only the core master plays.
- **FLAC form: the masters come first.** FLAC requires all metadata to precede the audio frames. As a result:
  - Windows' built-in decoder (Media Foundation, Explorer) does not recognise the file;
  - a player that reads the file directly over a network must load the masters before playback starts;
  - there is no seek table, so seeking in some players is slower than in a plain FLAC file.
- **WAV form: RF64 above 4 GiB.** Many WAV readers, including Android's, do not open RF64 files.
- **A 32-bit core master is not widely playable.** Many decoders do not play it: Windows and Android do not decode 32-bit FLAC. The reference encoder therefore uses a core master of at most 24 bit unless instructed otherwise.
- **Encoding takes time, memory and temporary disk space.** Encoding 30 minutes of music with 12 masters (FLAC form, on a 32-thread machine; [SPEC.md](SPEC.md), Section 11.5) took about 36 minutes, 1.4 GB of memory and about 28 GB of temporary disk space (2026-11-05).
- **The file is much larger than the core master alone.** It carries every master.

## Which form to use

- **FLAC form** (`.flac`, or `.lae` with `--form=flac`): the smallest form. The core master may be 24 bit, and tags are stored as in any FLAC file. Not suitable for Windows' built-in players.
- **WAV form** (`.wav`, or `.lae` with `--form=wav`): the core master in the `data` chunk (16 bit by default, which can be CD-compatible). Windows Explorer reads its length (tested); playback in Windows' built-in players was not tested. Larger, and uses RF64 beyond 4 GiB.
- **For streaming,** serve plain FLAC files made with `laes_export`, not the LAES file itself.
