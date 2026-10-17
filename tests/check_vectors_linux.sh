#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# Decoder conformance on Linux: builds src/lae_dec.cpp with several compilers / options and decodes every vector of
# tests/vectors/MANIFEST.md on all four routes; each output's SHA-256 (first 16 hex digits) must equal the manifest.
set -u
cd "$(dirname "$0")/.."
work=$(mktemp -d); fail=0; total=0
echo "host: $(uname -srm)"; g++ --version | head -1; clang++ --version | head -1
build() {   # $1 = label, rest = compiler and flags
    label=$1; shift
    if "$@" -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -o "$work/dec_$label" src/lae_dec.cpp 2> "$work/build_$label.txt"; then echo "build $label: OK"
    else echo "build $label: FAILED"; head -20 "$work/build_$label.txt"; fail=$((fail + 1)); fi; }
build gcc_O2 g++ -O2
build gcc_O0 g++ -O0
build clang_O2 clang++ -O2
case "$(uname -m)" in x86_64) build gcc_O2_m32 g++ -O2 -m32 -msse2 -mfpmath=sse ;; esac   # 32-bit SSE2 build: x86 hosts only
for exe in "$work"/dec_*; do [ -x "$exe" ] || continue; label=${exe##*/dec_}; bad=0; n=0
    sed '/^## Full SHA-256/,$d' tests/vectors/MANIFEST.md | grep '^| [a-z]' | while IFS='|' read -r _ name enc h24 h16 hcd hhalf _; do
        name=$(echo "$name" | tr -d ' '); rate=${name#*_}; rate=${rate%%.*}
        if [ $((rate % 44100)) -eq 0 ]; then base=44100; else base=48000; fi
        set -- "full $rate $h24" "16 $rate $h16" "cd24 $base $hcd" "full $((rate / 2)) $hhalf"
        for spec in "$@"; do route=${spec%% *}; rest=${spec#* }; orate=${rest%% *}; want=$(echo "${rest#* }" | tr -d ' ')
            rm -f "$work/o.wav"; "$exe" "tests/vectors/$name.lae.wav" "$work/o.wav" "$route" "$orate" > /dev/null 2>&1
            if [ -f "$work/o.wav" ]; then got=$(sha256sum "$work/o.wav" | cut -c1-16 | tr a-f A-F); else got="none"; fi
            case "$want" in [0-9A-F]*) ;; *) want="none" ;; esac
            if [ "$got" = "$want" ]; then echo "ok   $label $name $route $orate"; else echo "DIFF $label $name $route $orate: got $got want $want"; fi
        done
    done
done > "$work/results.txt"
cat "$work/results.txt" | grep -v '^ok' ; echo "summary: $(grep -c '^ok' "$work/results.txt") ok, $(grep -c '^DIFF' "$work/results.txt") differences, $fail build failures"
nd=$(grep -c "^DIFF" "$work/results.txt"); rm -rf "$work"
if [ "$nd" -gt 0 ] || [ "$fail" -gt 0 ]; then exit 1; fi
