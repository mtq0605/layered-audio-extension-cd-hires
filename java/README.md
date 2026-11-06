# LAE Java Library

[中文](README.zh.md)

This is a pure-Java implementation of the LAE encoders and decoders for Android and other Java platforms. It contains no native code. Its output is identical to that of the C++ reference tools from the same commit: the decoded samples are identical, and the encoded files are identical byte for byte.

## Contents

| Package | Contents |
|---|---|
| `com.lae.core` | Shared helpers: deterministic maths (`LMath`, the Java form of `src/lae_math.h`), half-band filters, alignment hash, little-endian I/O |
| `com.lae.laey` | LAE Lossy (LAEY): decoder, encoder (including independent-CD alignment, `noise=` and `g`), command-line tool `LaeEncTool` |
| `com.lae.laes` | LAE Lossless (LAES): decoder, player front end (`Play`), encoder, tags and pictures, command-line tool `LaesTool` |

The library does not include CD-Extra packaging (`lae_cdx`), disc images or the whole-disc planner (`lae_enc plan`).

## Requirements

- Java 8 (language level and library).
- Android: API level 24 (Android 7.0) or later. The library uses no API added after API level 24.
- No third-party dependencies.

## Building

```bash
javac -encoding UTF-8 --release 8 -d out/classes $(find src -name '*.java')
```

To check that no API newer than Android 7.0 is used, compile the sources a second time with `-source 8 -target 8 -bootclasspath <Android SDK>/platforms/android-24/android.jar`.

## Command-line use

The two tools take the same arguments and print the same messages as the C++ tools:

```bash
java -cp out/classes com.lae.laes.LaesTool enc out.flac --derive-core master.wav
```

```bash
java -cp out/classes com.lae.laes.LaesTool dec in.flac outdir
```

```bash
java -cp out/classes com.lae.laey.LaeEncTool master.wav out.wav
```

`LaesTool` supports `enc`, `dec` and `verify`. `LaeEncTool` supports the encoding options of `lae_enc` except `plan`. For the full argument lists, see the headers of `LaesTool.java` and `LaeEncTool.java`. Where `lae_enc` asks a question on the console (`bits=24`, `cdaccept`, `cdedge=`), the Java tool does not ask: it stops with the same message, and the option must be given.

## Equivalence with the C++ tools

The Java code is a port of the C++ reference in `src/`, which is not changed by the port. Each step of the port is checked by encoding and decoding the same inputs with both implementations and comparing the results byte for byte. Floating-point steps that can affect the output use `LMath`, which reproduces `lmath::` of the C++ tools exactly.

## Licence

The licences are the same as for the C++ code. The decoders and shared helpers are licensed under Apache-2.0. The encoder files are licensed under GPL-3.0-or-later or under a commercial licence (see [COMMERCIAL.md](../COMMERCIAL.md)). Each file names its licence in its SPDX header.
