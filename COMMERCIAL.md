<!--
SPDX-FileCopyrightText: 2026 mtq0605
SPDX-License-Identifier: CC0-1.0
-->

# Commercial licence for the LAE encoder

## Which files are dual-licensed

The encoder-side sources carry the SPDX expression
`GPL-3.0-or-later OR LicenseRef-LAE-Commercial`. These files are:

- in `src/`:
  - `lae_enc.cpp`
  - `pad_mirror.cpp`
  - `base_layer.cpp`
  - `routes.cpp`
  - `lossy_v2.cpp`
  - `part_coder.cpp`
  - `tone_scan.cpp`
  - `partition.h`
  - `cd_align.cpp`
  - `cd_plan.cpp`
  - `enc_io.h`
  - `blk_io.h`
  - `wav_in.h`
  - `pad_blk.cpp`
  - `base_blk.cpp`
  - `routes_blk.cpp`
  - `lossy_blk.cpp`
  - `part_blk.cpp`
  - `laes_codec_enc.h`, `laes_file_enc.h`, `laes_flac_enc.h`,
    `laes_stream_enc.h`, `laes_tags.h`, `laes_selftest.h`, `laes_tool.cpp`
    and `laes_export.cpp` (the encoder side of LAE Lossless (LAES),
    including the tools `laes` and `laes_export`)
- in `java/src/com/lae/`: the 27 encoder files of the Java library (the
  ports of the files above and their helpers, in `laey/` and `laes/`).
  Each of them names this expression in its SPDX header, and `AUTHORS`
  lists them by name.

You may use these files under the GNU GPL v3 or later. Alternatively, you
may use them under a separate commercial licence agreement with the author,
mtq0605.

Every other code file in the repository, including the decoder side, the
other tools in `src/` and `tools/`, the player plug-ins and the decoder
files of the Java library, is licensed under Apache-2.0 and never requires
a commercial licence. The SPDX header of each file (or, for a file
without one, `REUSE.toml`) is authoritative.

**In short:** most people never need a commercial licence. Encoding audio
with the encoder never requires one, whatever you do with the resulting
files.

## You do NOT need a commercial licence to

- **Encode audio** with the LAE encoder, for any purpose. This includes:
  - commercial releases;
  - selling or streaming the resulting files;
  - production services;
  - building a catalogue.

  This applies whether you use an official build or one you built yourself,
  modified or unmodified. The output is yours; see section 3 of `LICENSE`.
- **Use, embed or ship the decoder-side code** in any product, including
  closed-source software and hardware. That code is licensed under
  Apache-2.0. You must keep its copyright notices and pass on the `NOTICE`
  file.
- **Write your own encoder or decoder from the specification**, under any
  licence. See `PATENTS.md`.
- **Run the encoder on your own machines or servers.** This includes running
  a public online encoding service. Unlike the GNU AGPL, the GNU GPL v3 does
  not require you to release source code for use over a network.
- **Modify the encoder for your own internal use** without distributing it.
- **Distribute the encoder, modified or not, under the GNU GPL v3 or
  later**, even for a fee. You must provide the corresponding source code
  and keep the licence notices. Examples:
  - in a Linux distribution;
  - on a disc;
  - bundled with proprietary software, as a separate program that the
    software calls through the command line, files or pipes.

## You need a commercial licence if you distribute the encoder, or software containing it, without complying with the GNU GPL

Typical cases:

- **Linking into closed-source software.** You link encoder-side code,
  statically or dynamically, into a closed-source application, plug-in,
  SDK or firmware.
- **Restrictive distribution channels.** You distribute through a channel
  whose terms add restrictions that the GNU GPL v3 forbids (section 10).
  Examples include some app stores.
- **Locked-down devices.** You ship the encoder in a consumer device and
  cannot provide the Installation Information that section 6 of the GNU
  GPL v3 requires for "User Products". An example is a device with a locked
  bootloader.
- **Unpublished modifications.** You want to distribute a modified encoder
  without providing the source code of your modifications to its
  recipients.

A commercial licence is also available if you want commitments from the
author, such as warranty, indemnity or support, together with the licence.

If your design falls between these cases (for example, plug-in interfaces or
shared-memory IPC), please ask; you will receive a written answer.

## What the commercial licence covers

- **Encoder-side sources.** The licence covers the encoder-side sources,
  including contributions to them.
- **Contributions.** Contributions are accepted under the Apache License
  2.0, with a Developer Certificate of Origin sign-off on every commit (see
  `CONTRIBUTING.md`). The Apache License 2.0 allows the author to include
  them in the encoder under the GNU GPL and in its commercially licensed
  version. Contributors keep their copyright, and their contributions
  remain available under Apache-2.0.
- **Decoder-side files built into the encoder.** These are not covered by
  the commercial licence and need none: they remain licensed under
  Apache-2.0. You must keep their notices and pass on the `NOTICE` file.
- **Credit to contributors.** Licensees must keep the copyright notices in
  any source code they receive.
- **Names.** The licence grants no rights in the LAE names beyond truthful
  description of compatibility.
- **Patents.** The licence contains no patent licence. The author's patent
  pledge is in `PATENTS.md`.

## What it does not change

The GNU GPL version of the encoder remains available to everyone.

## How to ask

Enquiries are made through a public GitHub issue.

- **Open an issue** in the LAE repository with the title "Commercial licence
  enquiry".
- **Do not put confidential information in the issue.** Details are
  arranged afterwards.

Please include, as far as you can say in public:

- your company or name;
- the product;
- the target platforms;
- the distribution channel;
- the expected volume;
- whether you need to modify the encoder.

Terms are agreed case by case.
