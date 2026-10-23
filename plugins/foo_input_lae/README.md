# foo_input_lae: LAE decoder component for foobar2000

**English** | [中文](README.zh.md)

Plays LAE v5 files in foobar2000: the CD layer together with the depth layer and the frequency layer, decoded up to the master rate (for example 352.8 kHz / 24 bit).
A WAV file without an LAE extension is passed unchanged to foobar2000's own decoder.

- Hosts: the 32-bit versions of foobar2000 1.4, 1.5 (including the Chinese-localised edition) and 2.x. A 64-bit build for foobar2000 2.x will be made separately with the new SDK.
- Files: `.wav` (with an `LAE ` chunk after the `data` chunk) and `.lae` (the same file with another extension).
- Decoder: liblae (`src/lae_api.*` in this repository, Apache-2.0), compiled into this DLL; no other files are needed.

The screenshots are from the Chinese-localised edition of foobar2000 1.5.1 (a clean portable copy holding only the test files). When the system language is Chinese, all texts of the component are Chinese; otherwise they are English. The English texts are given below.

## Installation

Copy `foo_input_lae.dll` into the `components` folder of foobar2000 and restart foobar2000; LAE files then play.
The component is listed under Preferences → Components (in grey, which is normal: every component placed in the program folder is shown in grey).

![Component list](images/01_components.png)

## Decoder order: automatic, no dragging

An LAE file may be named `.wav`. foobar2000's built-in "foobar2000 WAV decoder" also accepts `.wav`, and whichever of the two decoders comes first in the list receives the `.wav` files.
If the WAV decoder comes first, an LAE `.wav` plays only its CD layer.

- A foobar2000 that has been in use for some time: foobar2000 places a newly installed decoder at the top of the list automatically, and nothing needs to be done.
- A newly installed foobar2000: the initial order of the list is random (in testing, it was different for every new configuration). If the WAV decoder happens to come before LAE,
  the component moves itself to the top when it starts and asks for one restart of foobar2000:

  ![Prompt](images/02_order_prompt.png)
  ![After the restart](images/03_order_fixed.png)

  How it works: foobar2000 places a decoder it has never seen at the top, so the component then uses its second identity (GUID). The old identity leaves a row at the bottom of the list,
  shown as "未命名旧解码器" ("unnamed old decoder") in the Chinese edition. That row accepts no files and can be ignored:

  ![The old row](images/04_old_entry.png)

  Answering "No" has the same effect; the change then applies the next time foobar2000 starts.
  The component adjusts the order in two cases only: when it first sees the order of a configuration (set randomly by foobar2000, or an order from a version before 0.3), and later when foobar2000
  places a newly installed decoder that also accepts .wav ahead of LAE (at most three times). If you drag LAE further down yourself, the component does not move it again. The automatic adjustment can be switched off in the advanced settings.
- `.lae` files are accepted only by LAE, whatever the order.
- Plain WAV files are not affected: LAE accepts only files that carry an LAE chunk. It rejects every other .wav (any sample rate and bit depth) at once, and foobar2000's built-in WAV decoder plays it unchanged.
  The picture shows the status bar for 6 plain WAV files and 1 LAE file, with LAE placed before the WAV decoder:

  ![Plain WAV](images/13_normal_wav.png)
- To check it yourself: Preferences → Playback → Decoding. The order is right when "LAE decoder" is above "foobar2000 WAV decoder":

  ![Decoder order](images/11_decoding_manual.png)

## Choosing the layers: menu Playback → LAE

| Menu item | What plays |
|---|---|
| All layers (master rate) | all layers, decoded to the master rate, for example 352.8 kHz / 24 bit (default) |
| All layers, up to 192 kHz | all layers, decoded to the highest rate step not above 192 kHz (352.8 kHz master → 176.4 kHz) |
| All layers, up to 96 kHz | all layers, decoded to the highest rate step not above 96 kHz (352.8 kHz master → 88.2 kHz) |
| CD layer + depth layer (24 bit) | 44.1 kHz / 24 bit |
| CD layer only (16 bit) | the CD unchanged, 44.1 kHz / 16 bit |

A selection made during playback applies at once and continues from the current position; the track does not start again. The first field of the status bar names the layers that play:

![Switching during playback](images/07_layer_switch.png)

The same settings are also under Preferences → Advanced → Decoding → LAE decoder, together with the switch "Put LAE ahead of decoders that also take .wav files (automatically)":

![Advanced settings](images/05_advanced.png)

## What is shown during playback

- Status bar: `LAE all layers` / `LAE CD + depth layer` / `LAE CD layer`, followed by the decoded sample rate.
- File properties → Details: sample rate, bit depth and codec LAE; under "Other": `lae_layers` (the layers the file contains), `lae_master` (the master rate) and
  `lae_cd_layer` (the format of the CD layer).
- Bit rate: LAE has a variable bit rate. The CD layer is fixed (1411.2 kbps in the 44.1 kHz family); the extension layers (depth layer + frequency layer) are entropy-coded and take more for complex content and
  less for quiet content. During playback, the status bar shows the bit rate actually used, updated once a second (the CD layer + the extension bytes read in that second); the first second and the file properties show
  the average of the whole file (file size ÷ duration). When only the CD layer plays, it is fixed at 1411. For the test excerpts, the extension layers averaged from about 10 kbps to about 2150 kbps.

  ![Live bit rate](images/12_live_bitrate.png)

![Playing](images/06_playing.png)
![Properties](images/08_properties.png)

When the extension is damaged (below, a test file in which one byte was changed on purpose), the whole track plays as the CD layer, the status bar shows "LAE CD layer (extension unusable)", and the console gives the reason:

![Status bar of a damaged file](images/09_damaged_status.png)
![Console of a damaged file](images/10_damaged_console.png)

## Rules (the same as for the reader)

- Before playback, the whole file is checked (the CRC of the extension chunk and the structure of the header and the streams). A file that fails is played entirely as the CD layer, and one line in the console gives the reason;
  the status bar then shows the format that actually plays (44.1 kHz / 16 bit), and the properties gain an entry `lae_route` with the reason.
- Integrity check (components that call the decoder with the testing-integrity flag): a file that can only be played as the CD layer is reported as an error.
- Within a track, the component never switches by itself between the CD layer and the extension; a stream error during playback is reported as an error and stops playback. Only a different selection in the menu changes the layers, at the current position.
- During decoding, the floating-point state is fixed (round to nearest, denormals not flushed to zero, x87 at 53-bit precision); afterwards the x87 and SSE states are restored separately.
- Memory: per track, only the CD layer and the extension block are kept, about the file size (version 0.1 needed 3.6 times that when opening a file). A 32-bit foobar2000 can use at most 4 GB,
  so a 352.8 kHz track longer than about one hour needs the 64-bit version.
- Seeking: a seek forward past the part not yet played must entropy-decode up to the target. On the test machine (i9-13900HX), this costs about 1.6 s per minute skipped at 352.8 kHz;
  a seek backwards, or into a part already played, is fast. v5 has no seek table; this is a limit of the format itself.

## Known issue: ESLyric 0.3.6 crashes

The lyrics component ESLyric 0.3.6 (2017), which comes with the Chinese-localised edition, crashes when the track changes, playback stops or foobar2000 closes while online lyrics are still being downloaded:
it accesses freed memory in `foo_uie_eslyric.dll` (`crash reports\failure_*.txt` names `eslyric_manager::_release_all`).
This is unrelated to LAE: it also happens without this component, with plain WAV files only, and it no longer happens once ESLyric is removed. Short test files of 10–15 s trigger it most easily.
Remedy: use a newer version of ESLyric, or remove it for the time being (Preferences → Components, right-click ESLyric, Remove, restart foobar2000).

## Building

Requires the foobar2000 1.5 SDK: `SDK-2019-12-27.7z` (576,451 bytes, SHA-256 `c9fe776096bcf2c5ec6f07788b4036c9146dd857bf3232053c13c1ee317e5ff6`,
from https://www.foobar2000.org/downloads/SDK-2019-12-27.7z). The SDK is not included in this repository.

```bat
plugins\foo_input_lae\build_fb2k.cmd <unpacked SDK folder> <output folder>
```

The x86 compiler of the VS2022 Build Tools is used. The SDK code is compiled with `/d2notypeopt`, as the SDK instructs; the LAE decoder uses its own verified options (`/fp:precise`).

## Licence

- Source code of this component: Apache-2.0.
- foobar2000 SDK compiled into the DLL: Copyright (c) 2001-2019 Peter Pawlowski, BSD-style licence (`sdk-license.txt`; redistributions of source code must retain the copyright notice,
  and the author's name must not be used to endorse products); pfc: zlib licence.
