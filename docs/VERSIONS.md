# Versions

[中文](VERSIONS.zh.md)

Each release states the version of every format and component that it contains. A component is identified by its own version number
where it has one, and otherwise by the commit of this repository from which it was built.

## Formats

| Format | Version | Specification | Status |
|---|---|---|---|
| LAE Lossy (LAEY) | container version 5 | [spec/lossy/SPEC.md](../spec/lossy/SPEC.md) | Stable |
| LAE Lossless (LAES) | payload version 2 (version 1 files remain valid) | [spec/lossless/SPEC.md](../spec/lossless/SPEC.md) | Stable |
| LAE CD-Extra encapsulation (CDX) | version 0 | [spec/cdx/SPEC.md](../spec/cdx/SPEC.md) | Draft |
| LAENOISE marking file | version 1 | [plugins/lae_noise_marker/SPEC_LAENOISE.md](../plugins/lae_noise_marker/SPEC_LAENOISE.md) | Stable |

The FLAC application IDs `LAE1` (LAEY) and `LAE2` (LAES) are registered with IANA, in the FLAC Application Metadata Block IDs registry (RFC 9639, Section 12.2).

## Components

| Component | Version | Reads / writes |
|---|---|---|
| `lae_enc`, `lae_dec` (LAEY encoder and decoder) | commit of the release | LAEY container v5, WAV and FLAC form, extension packs |
| `laes` (LAES tool) | commit of the release | LAES payload v1 and v2 |
| `lae_cdx` (CD-Extra tool) | commit of the release | CDX v0 |
| `lae_udec` (unified decoder) and `liblae` | commit of the release; `liblae` API 9 | LAEY v5 (both forms, extension packs), LAES v1 and v2 |
| `lae_noise` and the noise panel `noise_panel.html` (LAE Noise Marker) | commit of the release | LAENOISE v1 |
| foobar2000 component `foo_input_lae` | 0.30 | LAEY v5, LAES v1 and v2 |
| Kodi add-on `audiodecoder.lae` | 0.2.12 | LAEY v5, LAES v1 and v2 |
| DirectShow filter `dshow_lae` | 0.2.11 | LAEY v5, LAES v1 and v2 |
| VLC module `vlc_lae` | 0.1.11 | LAEY v5, LAES v1 and v2 |
| Java library (`java/`) | commit of the release | LAEY v5, LAES v1 and v2 |

## Compatibility

- Every component that reads a format version is meant to read every file of that version, within the known limits of the
  component and of the platform listed in [spec/COMPATIBILITY.md](../spec/COMPATIBILITY.md) and in the component's README. A later release of a component does not change
  the decoded output of an existing file.
- Encoders are deterministic: the same input, options and encoder commit produce the same file, byte for byte, on every supported
  platform (the builds listed in [BUILD.md](BUILD.md), Section 3). Encoder versions are therefore identified by commit. A change of encoder commit can change the encoded bytes, but it
  never changes the format version.
- The Java library contains encoders and decoders for LAEY and LAES. It produces the same encoded files and the same decoded samples
  as the C++ tools (`lae_enc`, `lae_dec`, `laes`) of the same commit.
- A format change that existing decoders cannot read raises the format version. For CDX, this rule applies from its first stable
  version; version 0 is a draft and may still change.

## Release table

Each release adds one row to this table.

| Release | Repository commit | LAEY | LAES | CDX | LAENOISE | foobar2000 | Kodi | DirectShow | VLC |
|---|---|---|---|---|---|---|---|---|---|
| v5-2026-11-07 | tag `v5-2026-11-07` | v5 | v1 | v0 | v1 | 0.20 | 0.2.5 | 0.2.4 | 0.1.4 |
| {NEXT_VERSION} | tag `{NEXT_VERSION}` | v5 | v2 | v0 | v1 | 0.30 | 0.2.12 | 0.2.11 | 0.1.11 |
