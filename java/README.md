# LAE Java Library

[中文](README.zh.md)

This is a pure-Java implementation of the LAE encoders and decoders for Android and other Java platforms. It contains no native code. Its output is identical to that of the C++ reference tools from the same commit: the decoded samples are identical, and the encoded files are identical byte for byte.

## Contents

| Package | Contents |
|---|---|
| `com.lae.core` | Shared helpers: deterministic maths (`LMath`, the Java form of `src/lae_math.h`), half-band filters, alignment hash, little-endian I/O |
| `com.lae.laey` | LAE Lossy (LAEY): decoder, encoder (including independent-CD alignment and the options `noise=` and `g`: noise marking and its second coding pass, [docs/NOISE.md](../docs/NOISE.md)), command-line tool `LaeEncTool` |
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

The two tools take the same arguments and print the same messages as the C++ tools, with the exceptions given below the examples:

```bash
java -cp out/classes com.lae.laes.LaesTool enc out.flac --derive-core master.wav
```

```bash
java -cp out/classes com.lae.laes.LaesTool dec in.flac outdir
```

```bash
java -cp out/classes com.lae.laey.LaeEncTool master.wav out.wav
```

`LaesTool` supports `enc`, `dec` and `verify`. `LaeEncTool` supports the encoding options of `lae_enc`; the whole-disc planner (`lae_enc plan`) is not included. For the full argument lists, see the headers of `LaesTool.java` and `LaeEncTool.java`. Where `lae_enc` asks a question on the console (`bits=24`, `cdaccept`, `cdedge=`), the Java tool does not ask: it stops with the message that `lae_enc` prints when it runs without a console, and the option must be given on the command line.

## Equivalence with the C++ tools

The Java code is a port of the C++ reference in `src/`, which is not changed by the port. Each step of the port is checked by encoding and decoding the same inputs with both implementations and comparing the results byte for byte. Floating-point steps that can affect the output use `LMath`, which reproduces `lmath::` of the C++ tools exactly.

The LAEY decoder interface (`LaeyDecoder`) has two additions that exist only in Java and are intended for phones. Neither changes the output:

- `lae_open_file(path, cdCacheBytes)` reads the CD layer from the file on demand instead of holding it in memory.
- `lae_dup()` makes a second track over a file that is already open. It shares the CD layer and the LAE chunk, which are not read or hashed again, and the seek checkpoints decoded so far. Its route, extension pack and decoding state are its own, so that a player can prepare a new route in a second thread while the old one continues to play.

Two differences in behaviour are specific to the Java port:

- The player front end `Play` offers the masters of LAES files with at most 16 masters (`Play.maxMasters`); a file with more masters plays its core master, and the reason is reported. The limit exists because a long chain of tiers needs more memory than a phone has (measured on a PC: about 1 MB of heap per master in the chain). The format allows 255 masters, and the limit can be raised when devices allow it.
- When several masters match `--pick=<rate>:<bits>` or `--pick=name:<text>`, `LaesTool` lists the candidates and writes nothing, as the C++ tool does outside a console; it has no tick list.

## Licence

The licences are the same as for the C++ code. The decoders and shared helpers are licensed under Apache-2.0. The encoder files are licensed under GPL-3.0-or-later or under a commercial licence (see [COMMERCIAL.md](../COMMERCIAL.md)). The player front end `Play` is a decoder file (Apache-2.0); the tag and picture code (`LaesTags`, `LaeRetag`) and the command-line tools (`LaesTool`, `LaeEncTool`) are encoder files. Each file names its licence in its SPDX header.
