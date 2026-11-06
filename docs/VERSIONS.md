# Versions

[中文](VERSIONS.zh.md)

Each release states the version of every format and component it contains. A component is identified by its own version number
where it has one, and otherwise by the commit of this repository it was built from.

## Formats

| Format | Version | Specification | Status |
|---|---|---|---|
| LAE Lossy (LAEY) | container version 5 | [spec/lossy/SPEC.md](../spec/lossy/SPEC.md) | Stable |
| LAE Lossless (LAES) | payload version 1 | [spec/lossless/SPEC.md](../spec/lossless/SPEC.md) | Stable |
| LAE CD-Extra encapsulation (CDX) | version 0 | [spec/cdx/SPEC.md](../spec/cdx/SPEC.md) | Draft |
| LAENOISE marking file | version 1 | [plugins/lae_noise_marker/SPEC_LAENOISE.md](../plugins/lae_noise_marker/SPEC_LAENOISE.md) | Stable |

The FLAC application IDs `LAE1` (LAEY) and `LAE2` (LAES) are registered with IANA.

## Components

| Component | Version | Reads / writes |
|---|---|---|
| `lae_enc`, `lae_dec` (LAEY encoder and decoder) | commit of the release | LAEY container v5 |
| `laes` (LAES tool) | commit of the release | LAES payload v1 |
| `lae_cdx` (CD-Extra tool) | commit of the release | CDX v0 |
| `lae_noise` and the noise panel | commit of the release | LAENOISE 1 |
| foobar2000 component `foo_input_lae` | 0.19 | LAEY v5, LAES v1 |
| Kodi add-on `audiodecoder.lae` | 0.2.4 | LAEY v5, LAES v1 |
| DirectShow filter `dshow_lae` | 0.2.3 | LAEY v5, LAES v1 |
| VLC module `vlc_lae` | 0.1.3 | LAEY v5, LAES v1 |
| Java library (`java/`) | commit of the release | LAEY v5, LAES v1 |

## Compatibility

- Every component that reads a format version reads every file of that version. A later release of a component does not change
  the decoded output of an existing file.
- Encoders are deterministic: the same input, options and encoder commit give the same file, byte for byte, on every supported
  platform. Encoder versions are therefore named by commit. A change of encoder commit can change the encoded bytes; it never
  changes the format version.
- The Java library produces the same encoded files and the same decoded samples as the C++ tools of the same commit.
- A format change that existing decoders cannot read raises the format version.

## Release table

Each release adds one row.

| Release | Repository commit | LAEY | LAES | CDX | LAENOISE | foobar2000 | Kodi | DirectShow | VLC |
|---|---|---|---|---|---|---|---|---|---|
| *next* | *set at release* | v5 | v1 | v0 | 1 | 0.19 | 0.2.4 | 0.2.3 | 0.1.3 |
