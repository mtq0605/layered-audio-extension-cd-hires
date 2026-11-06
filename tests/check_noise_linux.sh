#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# LAE Noise Marker on Linux (2026-11-06): builds lae_noise with GCC and Clang (and 32-bit x86 with SSE2 where the compiler can),
# generates the integer test signal of tools/gen_noise_test.cpp, scans it and compares the report and the candidate file with the
# Windows MSVC build (x64 and x86 gave the same bytes). Since 2026-11-06 the scanner uses no libm function that decides its
# output (src/lae_math.h), so every platform must write the same bytes.
set -u
cd "$(dirname "$0")/.."
w=$(mktemp -d); bad=0; F="-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread"
REF_WAV=0f39a72a1a9a3ab5dcaccf30fda806938f5a79a688e56126669dea7423806b6c
REF_TXT=6f10fe23328a4259c07ec1451daa9c06e036884adf70bca9ea9dcf376b5de2d5   # tests/noise_scan_ref.txt: the report without its first line (the input path)
REF_CAND=d242a92700555e0d213fc50d897102962335990997aed289027c8f783a74aaf8
g++ $F -o "$w/gen" tools/gen_noise_test.cpp 2> "$w/b_gen.txt" || { echo "build gen_noise_test: FAILED"; head -20 "$w/b_gen.txt"; exit 1; }
"$w/gen" "$w/t.wav" || { echo "test signal: FAILED"; exit 1; }
# the master's SHA-256 is that of the data chunk: the 44-byte header is skipped
got=$(tail -c +45 "$w/t.wav" | sha256sum | cut -c1-64); [ "$got" = "$REF_WAV" ] && echo "test signal: identical" || { echo "test signal: DIFFERENT ($got)"; bad=1; }
builds="g++ clang++"; [ "$(uname -m)" = "x86_64" ] && builds="$builds g++-m32"
for cc in $builds; do
    case $cc in g++-m32) cmd="g++ -m32 -msse2 -mfpmath=sse";; *) cmd=$cc;; esac
    $cmd $F -o "$w/ln" src/lae_noise.cpp 2> "$w/b.txt" || { echo "build lae_noise ($cc): FAILED"; head -20 "$w/b.txt"; bad=1; continue; }
    rm -f "$w/s.txt" "$w/c.laenoise"; "$w/ln" scan "$w/t.wav" "$w/s.txt" "$w/c.laenoise" > /dev/null 2>&1
    gc=$(sha256sum "$w/c.laenoise" 2>/dev/null | cut -c1-64)
    [ "$gc" = "$REF_CAND" ] && echo "scan ($cc): candidate file identical" || { echo "scan ($cc): candidate file DIFFERENT (${gc:-none})"; bad=1; }
    tail -n +2 "$w/s.txt" > "$w/s2.txt" 2>/dev/null; gt=$(sha256sum "$w/s2.txt" | cut -c1-64)
    [ "$gt" = "$REF_TXT" ] && echo "scan ($cc): report identical" || { echo "scan ($cc): report DIFFERENT ($gt)"; diff "$w/s2.txt" tests/noise_scan_ref.txt | head -10; bad=1; }
done
rm -rf "$w"; exit $bad
