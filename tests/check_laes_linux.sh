#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# LAE Lossless on Linux: builds laes and lae_cdx with g++ and clang++ and runs `laes selftest` (synthetic signals only: round trips over
# both carriers, all word lengths, both rate families, all storage values; damage and parser tests) and `lae_cdx selftest`.
set -u
cd "$(dirname "$0")/.."
w=$(mktemp -d); bad=0; F="-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread"
for cc in g++ clang++; do
    $cc $F -o "$w/laes" src/laes_tool.cpp 2> "$w/b.txt" || { echo "$cc build laes: FAILED"; head -20 "$w/b.txt"; bad=1; continue; }
    $cc $F -o "$w/lae_cdx" src/lae_cdx.cpp 2> "$w/c.txt" || { echo "$cc build lae_cdx: FAILED"; head -20 "$w/c.txt"; bad=1; continue; }
    "$w/laes" selftest > "$w/s.txt" 2>&1; r=$?; grep -E "FAIL|selftest:" "$w/s.txt"; [ $r -eq 0 ] || { echo "$cc: laes selftest FAILED"; bad=1; }
    "$w/lae_cdx" selftest > "$w/x.txt" 2>&1; r=$?; tail -1 "$w/x.txt"; [ $r -eq 0 ] || { echo "$cc: lae_cdx selftest FAILED"; bad=1; }
done
rm -rf "$w"; [ $bad -eq 0 ] && echo "LAES: all passed" || { echo "LAES: FAILED"; exit 1; }
