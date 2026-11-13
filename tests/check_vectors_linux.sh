#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# Decoder conformance on Linux: builds src/lae_dec.cpp with several compilers / options and decodes every vector of
# tests/vectors/MANIFEST.md on all four routes; each output's SHA-256 (first 16 hex digits) must equal the manifest.
set -u
cd "$(dirname "$0")/.."
work=$(mktemp -d); fail=0; built=0
echo "host: $(uname -srm)"; g++ --version | head -1; clang++ --version | head -1
build() {   # $1 = label, rest = compiler and flags
    label=$1; shift
    if "$@" -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -o "$work/dec_$label" src/lae_dec.cpp 2> "$work/build_$label.txt"; then echo "build $label: OK"; built=$((built + 1))
    else echo "build $label: FAILED"; head -20 "$work/build_$label.txt"; fail=$((fail + 1)); fi; }
build gcc_O2 g++ -O2
build gcc_O0 g++ -O0
build clang_O2 clang++ -O2
case "$(uname -m)" in x86_64) build gcc_O2_m32 g++ -O2 -m32 -msse2 -mfpmath=sse ;; esac   # 32-bit SSE2 build: x86 hosts only
# (2026-11-13) the builds decode at the same time, each into its own output and result file; the result files are joined in
# the order of the builds, so results.txt is the same as one build after the other
for exe in "$work"/dec_*; do [ -x "$exe" ] || continue; label=${exe##*/dec_}
    sed '/^## Full SHA-256/,$d' tests/vectors/MANIFEST.md | grep '^| [a-z]' | while IFS='|' read -r _ name enc h24 h16 hcd hhalf _; do
        name=$(echo "$name" | tr -d ' '); rate=${name#*_}; rate=${rate%%.*}
        if [ $((rate % 44100)) -eq 0 ]; then base=44100; else base=48000; fi
        set -- "full $rate $h24" "16 $rate $h16" "cd24 $base $hcd" "full $((rate / 2)) $hhalf"
        for spec in "$@"; do route=${spec%% *}; rest=${spec#* }; orate=${rest%% *}; want=$(echo "${rest#* }" | tr -d ' ')
            o="$work/o_$label.wav"; rm -f "$o"; "$exe" "tests/vectors/$name.lae.wav" "$o" "$route" "$orate" > /dev/null 2>&1
            if [ -f "$o" ]; then got=$(sha256sum "$o" | cut -c1-16 | tr a-f A-F); else got="none"; fi
            case "$want" in [0-9A-F]*) ;; *) want="none" ;; esac
            if [ "$got" = "$want" ]; then echo "ok   $label $name $route $orate"; else echo "DIFF $label $name $route $orate: got $got want $want"; fi
        done
    done > "$work/results_$label.txt" &
done; wait
for exe in "$work"/dec_*; do [ -x "$exe" ] || continue; cat "$work/results_${exe##*/dec_}.txt" 2>/dev/null; done > "$work/results.txt"
# Pass only when every decode is positively confirmed (2026-11-12): expected = MANIFEST rows (counted from the repository) x 4 routes
# x builds. A lost results file (temporary folder gone) used to leave the counts empty, and the script then exited 0.
rows=$(sed '/^## Full SHA-256/,$d' tests/vectors/MANIFEST.md | grep -c '^| [a-z]'); expected=$((rows * 4 * built))
nok=$(grep -c '^ok' "$work/results.txt" 2>/dev/null); nok=${nok:-0}; nd=$(grep -c '^DIFF' "$work/results.txt" 2>/dev/null); nd=${nd:-0}
grep -v '^ok' "$work/results.txt" 2>/dev/null; echo "summary: $nok ok, $nd differences, $fail build failures"
rm -rf "$work"
if [ "$nd" -eq 0 ] && [ "$fail" -eq 0 ] && [ "$built" -gt 0 ] && [ "$rows" -gt 0 ] && [ "$nok" -eq "$expected" ]; then exit 0; fi
echo "vectors: FAILED ($nok of $expected decodes confirmed)"; exit 1
