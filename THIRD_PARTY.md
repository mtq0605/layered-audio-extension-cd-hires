# Third-Party Components and Licences

[中文](THIRD_PARTY.zh.md)

All source code in this repository was written for the LAE project. No third-party source code is copied into it. The licences
of the project's own files are listed in [REUSE.toml](REUSE.toml) and [LICENSE](LICENSE); the origin of each encoder file is
recorded in [AUTHORS](AUTHORS).

Some components are built against third-party SDKs or headers. These are **not** part of this repository: the build scripts take
their location as an argument. When a built binary of such a component is distributed, the notices below apply to that binary.

## Command-line tools, reference decoders and encoders

| Dependency | Licence | Use |
|---|---|---|
| C++17 standard library and C runtime of the compiler (MSVC, GCC, Clang) | Compiler vendor's runtime terms | Linked; the Windows builds link the Microsoft C runtime statically (`/MT`) |
| Windows system libraries (`kernel32`, `user32`, `shell32`, `ole32`, `advapi32`) | Part of Windows | Linked on Windows only |
| Windows disc-image service IMAPI2 | Part of Windows | Used by `tools/laes_disc.ps1` through COM; nothing is distributed |

Independent implementations, no code taken from the projects named:

- the range coder (`src/part_stream.h`) is the project's own implementation; its bitstream equals that of an LZMA-style coder;
- the FFT and the DCT-IV (`src/fft.h`, `src/mdct_tb.h`) are the project's own code; the twiddle form follows the published
  literature;
- UTF-8 command-line handling on Windows (`src/win_utf8.h`) follows the approach of FLAC's `win_utf8_io`; no FLAC code is used.

## Player plug-ins

| Plug-in | Built against | Licence of that dependency | Effect on a distributed binary |
|---|---|---|---|
| foobar2000 (`foo_input_lae`) | foobar2000 SDK 2019-12-27 (x86) and 2026-10-01 (x64) | foobar2000 SDK licence (BSD-style) | Parts of the SDK are compiled into the component; ship the SDK licence text with the binary |
| VLC (`vlc_lae`) | VLC 3.0 SDK, `libvlccore` | LGPL-2.1-or-later | Dynamically linked; ship the LGPL notice and state where the VLC source is available |
| Kodi (`kodi_audiodecoder_lae`) | `kodi-dev-kit` headers, Kodi 21.3 "Omega" | GPL-2.0-or-later | The add-on binary is a combined work and is distributed under GPL-3.0-or-later (Apache-2.0 is compatible with GPL version 3, not version 2); the plug-in's own source stays Apache-2.0 |
| DirectShow (`dshow_lae`) | Windows SDK (`strmiids`, COM libraries) | Part of the Windows SDK | No further notice |
| LAE Noise Marker (`lae_noise_marker`) | Browser only (HTML and JavaScript) | — | No third-party code |

## Java library

The Java library (`java/`) uses only the Java standard library (Java 8 API level, Android API 24). It has no third-party
dependencies.

## Applications outside this repository

Applications that embed the LAE library, such as a mobile player, are separate works with their own licences and notices. Their
binaries are not distributed from this repository.

## Audio files in releases

The demonstration audio and masters attached to releases are the author's own works and are covered by `AUDIO_LICENSE` in each
release, not by the licences of this repository.
