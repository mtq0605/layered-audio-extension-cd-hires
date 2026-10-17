<!--
SPDX-FileCopyrightText: 2026 mtq0605
SPDX-License-Identifier: CC-BY-4.0
-->

# Building LAE

## 1. Compilers and options

All code is standard C++17 and uses no third-party libraries.

- The encoder is built from `src/lae_enc.cpp`.
- The decoder is built from `src/lae_dec.cpp`.
- The encoder uses `std::filesystem`. On systems other than Windows, it uses `setenv`/`unsetenv`.
- The source files of each program are listed in the [README](../README.md), Section 14.1.

| Platform | Compiler | Options |
|---|---|---|
| Windows x64 | MSVC 2022 | `cl /O2 /EHsc /std:c++17 /D_USE_MATH_DEFINES src\lae_dec.cpp` (encoder: add `/bigobj`) |
| Windows x86 (32-bit) | MSVC 2022 | as above; SSE2 is the MSVC 32-bit default |
| Linux / macOS x86-64 | GCC / Clang | `-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread` |
| Linux x86 (32-bit) | GCC / Clang | additionally `-msse2 -mfpmath=sse` (**required**) |
| ARM (32/64-bit) | GCC / Clang | `-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread` |

**Never use** `-ffast-math`, `/fp:fast` or any option that allows fused multiply-add contraction.

The output is bit-identical on every platform only if both of the following hold:

- the decoder's inverse MDCT is computed in strict IEEE-754 double precision, in source order;
- every constant is taken from the exact hexadecimal literals of `src/mdct_tables.h`.

**x87 builds are not supported.** The decoder's start-up self-test refuses them (Section 2).

Section 3 lists the builds that have been verified. Other builds, including all ARM builds, have not been verified.

Example (Linux, decoder and encoder):

```sh
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_dec src/lae_dec.cpp
g++ -O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread -o lae_enc src/lae_enc.cpp
```

### Auxiliary programs

The repository also contains the following programs. Each file states its usage in its header comment.

- `tools/selftest_ref.cpp` prints the self-test fingerprint of a build (Section 2).
- `tools/gen_halfband.cpp` and `tools/gen_mdct_tables.cpp` generate `src/halfband.h` and `src/mdct_tables.h`.
- `tools/test_mdct_tb.cpp` is a test program for `src/mdct_tb.h`.
- `src/gen_suite.cpp` generates the synthetic test signals.
- `src/cut_wav.cpp` and `src/cmp_wav.cpp` are WAV utilities. `src/cmp_wav.cpp` computes the error measures of README Section 12.1.

## 2. Start-up self-test

At every start, the decoder passes a fixed data pattern through the inverse MDCT and hashes the result.

- **Hash.** The hash is a 64-bit FNV-1a-style hash with a non-standard offset basis ([SPEC.md](../spec/lossy/SPEC.md), Section 13).
- **Expected value.** The hash must equal `0x729b547fbdbfe203` (`SELFTEST_REF` in `src/mdct_tb.h`).
- **On mismatch.** If the hash differs, the build has changed the floating-point behaviour. The decoder then refuses to run (exit code 4) rather than produce output that differs from other platforms.

`tools/selftest_ref.cpp` prints the hash of a build.

## 3. Verified builds

| Build | Decoder: 16 vectors x 4 routes | Encoder: 16 vectors |
|---|---|---|
| MSVC 2022 x64 `/O2` and `/Od`, x86 SSE2 (Windows) | identical | reference files |
| GCC 13 x86-64 `-O2` and `-O0` (Linux) | identical | identical to the reference files, byte for byte |
| Clang 18 x86-64 `-O2` (Linux) | identical | not checked |
| GCC 13 x86 32-bit SSE2 (Linux) | identical | not checked |

On Linux, 256 of 256 decoder results were identical to the reference.

The Linux checks are made with two scripts:

- `tests/check_vectors_linux.sh` checks the decoder;
- `tests/check_encoder_linux.sh` checks the encoder.

The scripts print a summary of the comparisons. A GitHub Actions workflow in `.github/workflows/` runs them.

The conformance vectors and their hashes are listed in [tests/vectors/MANIFEST.md](../tests/vectors/MANIFEST.md).

## 4. Threads

- **Encoder.** The encoder's partition search runs in parallel over root blocks.
  - The number of threads defaults to the number of cores reported by the system (`std::thread::hardware_concurrency`).
  - The environment variable `PARTTHREADS` sets it explicitly.
  - The thread count does not affect the result: 1 and 16 threads produce byte-identical streams.
- **Decoder.** The decoder is single-threaded.

## 5. Speed and memory

The following values were measured during development (2026-10-16) on Windows x64, with a 352.8 kHz stereo file of 40 s.

| | Time | Peak memory |
|---|---|---|
| Decoding (24-bit route, streaming) | 2.8 s (about 14x real time) | 38 MB |
| Encoding (mode 3, 16 threads) | 113 s (about 0.35x real time) | 1.35 GB |

**Decoder.** The decoder is streaming, and its memory does not grow with track length, apart from the input file, which is currently read whole. It:

- decodes one row of root blocks (8 frames) at a time;
- overlap-adds the inverse MDCT frame by frame;
- pulls every resampling stage through short sliding windows.

**Encoder.** The encoder processes a whole track, because the mode proportions are measured over the whole track. Its memory grows linearly with the length. A streaming encoder is future work.
