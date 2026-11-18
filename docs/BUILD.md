<!--
SPDX-FileCopyrightText: 2026 mtq0605
SPDX-License-Identifier: CC-BY-4.0
-->

# Building LAE

[中文](BUILD.zh.md)

## 1. Compilers and options

All code is standard C++17 and uses no third-party libraries.

- The encoder is built from `src/lae_enc.cpp`.
- The decoder is built from `src/lae_dec.cpp`.
- The CD-Extra tool `lae_cdx` is built from `src/lae_cdx.cpp`.
- The LAE Lossless tool `laes` (encoder, decoder, alignment and self-test) is built from `src/laes_tool.cpp`. LAES computes its reference taps in IEEE-754 double precision, in source order ([spec/lossless/SPEC.md](../spec/lossless/SPEC.md), Section 2.2). Build `laes` and `lae_cdx` with `/fp:strict` (MSVC) or `-ffp-contract=off` (GCC / Clang).
- The decoder library `liblae` is built from `src/lae_api.cpp`.
- The foobar2000 component is built with `plugins/foo_input_lae/build_fb2k.cmd` ([plugins/foo_input_lae/README.md](../plugins/foo_input_lae/README.md)).
- The encoder uses `std::filesystem`. On systems other than Windows, it also uses `setenv`/`unsetenv`.
- The source files of each program are listed in the [README](../README.md), Section 14.1.
- On Windows, file names and tags outside the system code page need Windows 10 version 1803 or later (the tools use the UTF-8 locale, `src/win_utf8.h`); on older versions of Windows, the tools print a warning and work in the system code page.

| Platform | Compiler | Options |
|---|---|---|
| Windows x64 | MSVC 2022 | `cl /O2 /EHsc /std:c++17 /utf-8 /D_USE_MATH_DEFINES src\lae_dec.cpp` (encoder: add `/bigobj /fp:strict`) |
| Windows x86 (32-bit) | MSVC 2022 | as above; SSE2 is the MSVC 32-bit default |
| Linux / macOS x86-64 | GCC / Clang | `-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread` |
| Linux x86 (32-bit) | GCC / Clang | additionally `-msse2 -mfpmath=sse` (**required**) |
| ARM (32/64-bit) | GCC / Clang | `-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread` |

**Never use** `-ffast-math`, `/fp:fast` or any other option that allows fused multiply-add contraction.

**MSVC must always be given `/utf-8`.** The source files are UTF-8 without a byte-order mark, and some of them contain Chinese text in string literals (for example, the `README.TXT` and `BURN.TXT` that `lae_cdx` writes to a disc, and the Chinese texts of the plug-ins). Without `/utf-8`, MSVC reads a source file in the code page of the system; on a Chinese Windows this is GBK (code page 936), and the text is then damaged without any effect on the audio:

- in a plain literal (`"..."`), a byte that does not pair under GBK is replaced by `?`, so that a character is cut off;
- in a `u8"..."` or `L"..."` literal, the text is converted to other characters, with no `?`;
- warning C4819 appears only for characters that GBK cannot represent, so a build can be damaged without any warning.

Give `/utf-8` to every MSVC call, including those for files that are plain ASCII today (or set the environment variable `CL=/utf-8`). GCC and Clang read UTF-8 by default; `javac` is called with `-encoding UTF-8`.

**Checks after a release build.** Before the programs are packed:

1. Treat warning C4819 in any compiler log as an error (or compile with `/we4819`).
2. Search every program, including the DLLs inside the plug-in packages, for damaged Chinese text: a Chinese character whose last byte has become `?`, and the known Chinese phrases of the sources in their damaged `u8` / `L` form. A Chinese build of a plug-in must contain its Chinese texts in the correct form.
3. Check that every text file the programs write (for example, the disc `README.TXT` and `BURN.TXT`, also inside the disc image and `DATA.ISO`) is valid UTF-8 and free of the damage above.

The output is bit-identical on every platform only if both of the following conditions hold:

- the decoder's inverse MDCT is computed in strict IEEE-754 double precision, in source order;
- every constant is taken from the exact hexadecimal literals in `src/mdct_tables.h`.

**x87 builds are not supported.** The decoder's start-up self-test rejects them (Section 2).

**The encoder is also bit-identical on every platform (2026-11-03).** Everything that determines its output (code lengths, detector windows and thresholds, filter taps, the FFT and the alignment) uses the functions in `src/lae_math.h` (`log2`, `sinpi`, `cospi`, `exp2`, `log10`) instead of those of the C library. These functions use only the IEEE-754 operations `+ − × ÷`, `sqrt`, `floor`, `frexp` and `ldexp`, in a fixed order, in the same way as LAES does in `src/laes_math.h`. Equal tone frequencies are merged in the order in which they were found (`std::stable_sort`). Build the encoder with the flags of the table above (the decoder's flags plus `/fp:strict` on MSVC; `-ffp-contract=off` on GCC / Clang; SSE2 on 32-bit x86): the x64 and x86 builds then write byte-identical files, and `tests/check_encoder_*` treat any file that differs as a failure. `tools/lae_math_check.cpp` measures these functions against the C library (at most 3 units in the last place for `log2`) and prints a checksum that must be the same on every platform. Before 2026-11-03, the C library's `log2` could differ in the last bit between platforms and change the choice between two partitions of equal cost; the decoded audio was the same.

Section 3 lists the builds that have been verified. Other builds, including 32-bit ARM and macOS on ARM, have not been verified.

Example (Linux, decoder and encoder):

```sh
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_dec src/lae_dec.cpp
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_enc src/lae_enc.cpp
g++ -O2 -std=c++17 -ffp-contract=off -pthread -o laes src/laes_tool.cpp && ./laes selftest
```

Example (Windows, `lae_cdx` and `laes`; `/fp:strict` is required for the LAES reference taps):

```bat
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /fp:strict /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /bigobj src\lae_cdx.cpp
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /fp:strict /D_CRT_SECURE_NO_WARNINGS src\laes_tool.cpp /Fe:laes.exe
laes selftest
```

Example (Windows, the unified decoder `lae_udec`, which links `liblae` statically with the decoders of both formats):

```bat
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /fp:strict /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /DLAE_STATIC /bigobj src\lae_udec.cpp src\lae_any.cpp src\lae_api.cpp src\laes_api.cpp /Fe:lae_udec.exe /link /MANIFEST:EMBED /MANIFESTINPUT:src\win_longpath.manifest
```

Example (Windows, `lae_repair`, which puts a file that another program changed back into the LAE form; the same sources as `lae_udec`; on Linux, the same sources with g++ and `-DLAE_STATIC`):

```bat
cl /nologo /O2 /EHsc /std:c++17 /utf-8 /fp:strict /D_USE_MATH_DEFINES /D_CRT_SECURE_NO_WARNINGS /DLAE_STATIC /bigobj src\lae_repair.cpp src\lae_any.cpp src\lae_api.cpp src\laes_api.cpp /Fe:lae_repair.exe /link /MANIFEST:EMBED /MANIFESTINPUT:src\win_longpath.manifest
```

`liblae` (the LAE Lossy decoder, `src/lae_api.cpp`) is compiled with `/fp:precise`, which meets the floating-point rules of [spec/lossy/SPEC.md](../spec/lossy/SPEC.md), Section 2.2: no contraction, because the build scripts (MSVC 2022) pass no `/arch:AVX2` or higher option, so the compiler has no fused multiply-add instruction to emit. Do not add such an option. The LAE Lossless decoder of the plug-ins (`src/laes_api.cpp`) is compiled with `/fp:strict`. The verified flags are given in the build scripts of the plug-ins, for example `plugins/foo_input_lae/build_fb2k.cmd`.

### Auxiliary programs

The repository also contains the following programs. The header comment of each file describes its usage.

- `tools/selftest_ref.cpp` prints the self-test fingerprint of a build (Section 2).
- `tools/gen_halfband.cpp` and `tools/gen_mdct_tables.cpp` generate `src/halfband.h` and `src/mdct_tables.h`, respectively. The committed `src/mdct_tables.h` is the normative table: the generator writes its result into the current folder, only for comparison. Built with x64 MSVC, it reproduces the committed table exactly; built for x86 (`/arch:SSE2`), 75 values differ in the last bit (measured 2026-11-14), and other compilers or C libraries may differ as well, because the generator uses the C library's `sin` and `cos`. Never commit a regenerated table: every decoder must use the same constants. The generators (and `gen_noise_test`) do not replace an existing output file silently: on a console they ask twice and replace it only after two yes answers; otherwise, it is refused unless `--overwrite` is given (README, Section 14.11).
- `tools/test_mdct_tb.cpp` is a test program for `src/mdct_tb.h`.
- `tools/lae_math_check.cpp` checks the encoder's functions in `src/lae_math.h` against the C library and prints their platform checksum.
- `src/gen_suite.cpp` generates the synthetic test signals (optionally a single category).
- `src/cd_variant.cpp` derives exactly defined test variants of a CD or master (shift, padding, cut, gain, polarity, channel swap, fade, and others) for the alignment and extension-pack tests (`tests/check_packs.sh`).
- `tools/lae_abx/lae_abx.cpp` is the stand-alone ABX listening test (Windows x64, Windows 10 version 1703 or later: it uses the per-monitor DPI functions of that version). It needs `lae_udec.exe` next to it. Build (x64): `cl /nologo /O2 /EHsc /std:c++17 /utf-8 /DUNICODE /D_UNICODE tools\lae_abx\lae_abx.cpp user32.lib gdi32.lib comctl32.lib ole32.lib shell32.lib mfplat.lib mfreadwrite.lib mfuuid.lib winmm.lib comdlg32.lib`. Prepared sources are kept in `%TEMP%\lae_abx_cache` together with the identity of `lae_abx` and of the `lae_udec.exe` next to it, so a copy made by another version is prepared again; the button "Clear kept copies" removes them. The log of a pair is written from its start, one line per answer, and the session summary gives the test rate of every pair. `lae_abx --prepare <pair> <rate> <folder> [--overwrite]` does not replace existing outputs without `--overwrite`.
- `src/cut_wav.cpp` and `src/cmp_wav.cpp` are WAV utilities. `src/cmp_wav.cpp` computes the error measures defined in README Section 12.1.

## 2. Start-up self-test

At every start, the decoder passes a fixed data pattern through the inverse MDCT and hashes the result.

- **Hash.** The hash is a 64-bit FNV-1a-style hash with a non-standard offset basis ([SPEC.md](../spec/lossy/SPEC.md), Section 13).
- **Expected value.** The hash must equal `0x729b547fbdbfe203` (`SELFTEST_REF` in `src/mdct_tb.h`).
- **On mismatch.** If the hash differs, the build has changed the floating-point behaviour. The decoder then refuses to run (exit code 4) rather than produce output that differs from that of other platforms.

`tools/selftest_ref.cpp` prints the hash of a build.

## 3. Verified builds

| Build | Decoder: 16 vectors × 4 routes | Encoder: 16 vectors |
|---|---|---|
| MSVC 2022 x64 `/O2` and `/Od`, x86 SSE2 (Windows) | identical | reference files (the vectors were written with these builds; x64 and x86 write identical files, Section 1) |
| GCC 13 x86-64 `-O2` and `-O0` (Linux) | identical | identical to the reference files, byte for byte |
| Clang 18 x86-64 `-O2` (Linux) | identical | not checked |
| GCC 13 x86 32-bit SSE2 (Linux) | identical | not checked |
| GCC 13 ARM64 (aarch64) `-O2` and `-O0` (Linux) | identical | `-O2`: identical to the reference files, byte for byte |
| Clang 18 ARM64 (aarch64) `-O2` (Linux) | identical | not checked |

On Linux x86, 256 of 256 decoder results were identical to the reference; on Linux ARM64, 192 of 192 were.

`liblae` and the foobar2000 component have been built and tested only on Windows with MSVC 2022 (README, Sections 13.3 and 13.7). `laes` and `lae_cdx` are not in the table: on Linux, the CI builds both with g++ and clang++, on x86-64 and on ARM64, and runs `laes selftest` and `lae_cdx selftest` (`tests/check_laes_linux.sh`); their other commands are not run by the CI there. On 2026-11-14, the `laes` commands `dec`, `verify`, `extract` and `add`, and the `lae_cdx` reader (`read` and `repack`, with WAV and FLAC rips), were run once on Linux x64 (g++ 13.3 under WSL), with output byte-identical to that of the Windows build; `laes enc` and `laes align` were not checked there ([spec/lossless/LAES_TOOL.md](../spec/lossless/LAES_TOOL.md), section "Building").

The Linux checks are performed by six scripts:

- `tests/check_vectors_linux.sh` checks the decoder;
- `tests/check_encoder_linux.sh` checks the encoder;
- `tests/check_laes_linux.sh` builds `laes` and `lae_cdx` and runs their self-tests;
- `tests/check_noise_linux.sh` checks the noise scan;
- `tests/check_tags_linux.sh` checks tags and pictures;
- `tests/check_repair_linux.sh` builds `lae_udec` and `lae_repair` and repairs LAEY and LAES files that `flac` re-encoded or packed (since 2026-11-19; it needs the `flac` command).

The scripts print a summary of the comparisons. A GitHub Actions workflow in `.github/workflows/` runs them.

**Checks run by hand (2026-11-15).** The workflow does not run the following checks; they are run by hand on a local machine before a release, and they will be added to the workflow when it is next changed:

- `tests/check_packs.sh <bin folder> <older lae_dec> <scratch folder>`: extension packs (Windows with Git Bash, or Linux);
- `tests/check_flacform.sh <lae_enc> <lae_dec> <master.wav> <scratch folder>`: the FLAC form;
- `tools/lae_math_check`: the encoder's functions of `src/lae_math.h` against the C library, with a checksum that must be the same on every platform;
- the 32-bit (x86) build of `laes` and its `laes selftest`.

The conformance vectors and their hashes are listed in [tests/vectors/MANIFEST.md](../tests/vectors/MANIFEST.md).

## 4. Threads

- **Encoder.** The encoder's partition search runs in parallel over root blocks.
  - By default, the number of threads is the number of hardware threads (logical processors) reported by the system (`std::thread::hardware_concurrency`).
  - The test switch `PARTTHREADS` sets it explicitly; like every test switch of `lae_enc` (`LAEY_TEST_PIECE`, `LAE_TESTMAXFRAMES`, `LAE_TESTFAIL`, `LAE_BLOCKUNITS`, `LAE_TESTSEQUENTIAL`, `PARTTHREADS`), it acts only when `LAE_TEST_MODE=1` is set, and every run then prints one line naming the active switches. `TIMING`, `KEEPTMP` and `TIERSTAT` change no output byte and act without it (README, Section 14.11).
  - The thread count does not affect the result: 1 and 16 threads produce byte-identical streams.
  - Since 2026-10-29, the filters of the route split are also divided across threads, and the two layers of a pair (the depth layers L and R, the frequency layers M and S, and L and R again after leak compensation) can be coded at the same time. Every output value is computed with the same operations in the same order, so the output is byte-identical.
  - Memory plan: at start-up, the encoder reads the physical memory size once and decides from the track length whether the two layers of each pair run at the same time or one after the other (`memory plan: …`). They run at the same time only on 64-bit builds, only if `PARTTHREADS` is not 1, and only if about 2 × (1 byte per layer sample + 128 MB) fits within both the memory limit and the available memory. The limit is set with `mem=<GB>`; by default it is min(1/2 × physical memory, max(1/4 × physical memory, physical memory − 28 GB)): 8 GB on a 32 GB machine, 20 GB on 48 GB and 32 GB on 64 GB. The block length is the largest that fits within the limit, up to 4 units; a unit is lcm(seg, 8) MDCT frames, where a floor segment seg is about one second of frames, so 4 units are about 32 s at 352.8 kHz. `LAE_BLOCKUNITS` sets the number of units manually (a test switch, only with `LAE_TEST_MODE=1`; the 512 s blocks below were set this way). Measured on a 10.16-minute 352.8 kHz test master (unpublished): 32 s blocks, 767 s / 471 MB; 24 s blocks (`mem=1`), 776 s / 408 MB; 512 s blocks, 803 s / 6,921 MB; all outputs were byte-identical. On Linux, the encoder reads `/proc/meminfo` (MemAvailable); where the memory size cannot be read, the layers run one after the other.
  - `PARTTHREADS=1` makes the whole encoder single-threaded.
  - An error in any thread (out of memory, a failed write) is passed to the main thread and handled as an error in the main thread is: no output is written and the temporary folder is removed (with `resume`, it is kept for the next run; README Section 14.2).
- **Decoder.** The decoder is single-threaded.

## 5. Speed and memory

The following values were measured during development (2026-10-16) on Windows x64, with a 40 s stereo file at 352.8 kHz.

| | Time | Peak memory |
|---|---|---|
| Decoding (24-bit route, streaming) | 2.8 s (about 14× real time) | 38 MB |
| Encoding (mode 3, 16 threads; encoder of 2026-10-16) | 113 s (about 0.35× real time) | 1.35 GB |

Encoder of 2026-10-29 (Windows x64, 32 threads, 352.8 kHz stereo, 60 s, mode 3, s1; output byte-identical to that of the 2026-10-24 encoder):

| Encoder | Time | Peak memory |
|---|---|---|
| 2026-10-24 | 177 s | 1.83 GB |
| 2026-10-29, layers one after the other | 59 s | 1.06 GB |
| 2026-10-29, two layers of a pair at the same time | 43 s | 1.49 GB |

Block-wise encoder of 2026-10-29 (same machine, mode 3, s1; output byte-identical to that of the whole-track encoder):

| Master | Time | Peak memory |
|---|---|---|
| 352.8 kHz stereo, 60 s | 127 s (machine under load from other runs) | 482 MB |
| 352.8 kHz 32-bit stereo, 10.16 min (bits=24) | 492 s | 475 MB (whole-track encoder: 580 s with the layers one after the other, 12,039 MB) |
| 352.8 kHz stereo, 13 min (beyond the former Windows limit of about 12.7 minutes at 352.8 kHz) | 961 s | 733 MB (measured with the build of 2026-10-29, before the `LAE ` chunk was written without a second copy of the streams) |

For the whole-track encoders above, time and memory grew linearly with length (measured at 10, 20, 40 and 60 s). Most of the time saved comes from the partition search, which previously allocated a cut list for every rectangle; with many threads, these heap allocations waited on each other. The memory savings come from three changes: one reconstructed plane is kept instead of two identical copies, each octave segment holds only its own bins, and signals are freed as soon as they are no longer needed.

**Decoder.** The decoder is streaming, and its memory use does not grow with track length, apart from the input file, which is currently read whole. The decoder:

- decodes one row of root blocks (8 frames) at a time;
- overlap-adds the inverse MDCT frame by frame;
- pulls every resampling stage through short sliding windows.

**Encoder (block-wise, 2026-10-29).** The mode proportions and the other track-wide decisions are measured over the whole track, but the track is read and coded block by block:

- the master is read piece by piece (WAV, RF64, Sony Wave64);
- the CD layer, the route split and the layer coder work on blocks of whole floor segments and whole root rows (about 32 s), and each block is read with the margins that its filters need; every value is computed with the same operations in the same order as before, so the output is byte-identical;
- the scans accumulate whole-track summaries (floors, tone and transient events, mean energies, code-length statistics, shares), the plan (tags, lambdas) is made from these summaries, and every block is then coded according to the plan;
- the range coder and its self-check work on one row of root blocks at a time, and the output file is written and read back piece by piece.

Without `cd=`, memory use does not grow with length, apart from the per-cell records (3 bytes per cell); for `cd=`, see below. Large intermediate data are written to the work folder: at most 13,185 MB was measured for a 10.16-minute 352.8 kHz 32-bit master (an unpublished test master; bits=24, mode 3, s1; 823 s and 465 MB peak with 8 threads), which is about 61 bytes per master frame. Since 2026-11-05, the work folder has been smaller (8.0 GB for a master of that length and rate; README Section 14.2), and the free-space check before the run assumes 45 bytes per master frame (55 with `cd=`, and 12 more with `g`). With `cd=`, the master is also read through file windows, and the placement on the CD's time line (the digital silence added around the shifted master, the whole-sample shift itself and the cut of that silence) is written from file to file; the alignment search itself, however, correlates over the whole track at the CD rate (about 2.4 GB for 10 minutes). The option `resume` continues an interrupted run from its last finished step (with `cd=`, it continues after the alignment, which is performed again).

**Decoder input.** The decoder reads the CD audio of its input file in place (one copy, 2 bytes per CD sample).

**LAE Lossless (`laes`, block-wise, 2026-10-30).** `laes enc`, `dec`, `verify` and `align` work on pieces of the base level (all rate levels of a piece at once); the output is byte-identical to that of the whole-track `laes` of 2026-10-29. Each storage candidate is written to a work folder (`<output>.laes.tmp`, which is removed at the end; `KEEPTMP=1` keeps it), and only the smallest is kept; the WAV and FLAC files are written and read back piece by piece. The memory limit is set with `--mem=<GB>`; by default it is 2 GB, or less where min(1/2 × physical memory, max(1/4 × physical memory, physical memory − 28 GB)) is smaller (since 2026-11-04). The measurements below were made on 2026-10-30, when the default was min(1/2 × physical memory, max(1/4 × physical memory, physical memory − 28 GB)) without the 2 GB cap. A WAV output larger than 4 GiB is written in the RF64 form ([spec/lossless/SPEC.md](../spec/lossless/SPEC.md), Section 7).

Measured on Windows x64 (32 threads, 32 GB):

| Content | Step | Time | Peak memory |
|---|---|---|---|
| 60 s, four masters (32 / 24 / 16 bit at 352.8 kHz, 24 bit at 176.4 kHz) | encode, whole-track `laes` | 256 s | 5,743 MB |
| | encode, block-wise | 236 s | 1,017 MB (`--mem=1`: 373 MB) |
| | decode, whole-track / block-wise | | 4,704 MB / 773 MB |
| 10:09 (unpublished test master), core master: an independent CD, the same four masters | encode, FLAC form | 1,661 s | 2,860 MB |
| | encode, WAV form | 1,166 s | 2,859 MB |
| | decode and verify | about 175 s | 2,239 MB |
| | encode, FLAC form, `--mem=20` (8 threads) | 3,585 s | 6,553 MB |
| | encode, FLAC form, `--mem=2` (8 threads) | 3,655 s | 1,034 MB |

All encode rows of the same form give the same file. **Disk space:** the work folder for the 10-minute, four-master case reached 38.6 GB (measured; the same for `--mem=20` and `--mem=2`), which is about 3.8 GB per minute of music for these four masters; the folder is removed at the end. Keep that much free space on the drive of the output file, plus space for the output itself.
