#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# Encoder on Linux: regenerates the four synthetic source clips of the conformance vectors, encodes them with the 4 vector
# configurations and compares with tests/vectors/MANIFEST.md (encoded file and 24-bit decode). The decoder must match exactly;
# the encoder must match too: since 2026-11-03 it uses no libm function in anything that decides its output (src/lae_math.h), so every
# platform writes the same bytes; a different file is a failure.
set -u
cd "$(dirname "$0")/.."
w=$(mktemp -d); bad=0; F="-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread"
for p in lae_enc lae_dec gen_suite cut_wav; do g++ $F -o "$w/$p" src/$p.cpp 2> "$w/b_$p.txt" || { echo "build $p: FAILED"; head -20 "$w/b_$p.txt"; exit 1; }; done
echo "builds: OK"
# full SHA-256 of the generated 20-s sources (Windows MSVC x64 and x86 build): since 2026-11-07 gen_suite uses lmath only
# (src/lae_math.h), so the whole generated file is the same on every platform; a different source is a failure
for spec in "sweep_88200 814A6B7E392C8BF35E424B145F0971D4804A0EF3DE19B248387F865E22651606" \
            "transients_176400 1D4FA24ADC8C2268443212D1070DB650D54A8ED878A9D67D7C9827736B6D1743" \
            "imd_96000 CE5419A2E8EED5D0AD45F681E5285D3C80CFFD66EFE92021313DEB81870909FA" \
            "electronic_352800 40D8F8D053F5AD175062052055BD0659973C28DDA824286BF4144B1080A2373D"; do
    c=${spec% *}; ref=${spec#* }; rate=${c#*_}
    [ -f "$w/$c.wav" ] || "$w/gen_suite" "$w" "$rate" > /dev/null
    got=$(sha256sum "$w/$c.wav" | cut -c1-64 | tr a-f A-F)
    if [ "$got" = "$ref" ]; then echo "source $c: identical"; else echo "source $c: DIFFERENT ($(echo "$got" | cut -c1-16))"; echo x >> "$w/bad"; fi
    "$w/cut_wav" "$w/$c.wav" "$w/$c.2s.wav" 5 2 > /dev/null
done
sed '/^## Full SHA-256/,$d' tests/vectors/MANIFEST.md | grep '^| [a-z]' | while IFS='|' read -r _ name enc h24 _; do
    name=$(echo "$name" | tr -d ' '); enc=$(echo "$enc" | tr -d ' '); h24=$(echo "$h24" | tr -d ' ')
    clip=${name%%.*}; cfg=$(echo "${name#*.}" | tr '.' ' ')
    log=$("$w/lae_enc" "$w/$clip.2s.wav" "$w/e.lae.wav" $cfg 2>&1); ok=$(echo "$log" | grep -c 'LAE encode OK')
    if [ "$ok" -eq 0 ] || [ ! -f "$w/e.lae.wav" ]; then echo "encode $name: FAILED"; echo "$log" | tail -5; echo x >> "$w/bad"; continue; fi
    ge=$(sha256sum "$w/e.lae.wav" | cut -c1-16 | tr a-f A-F)
    rm -f "$w/o.wav"; "$w/lae_dec" "$w/e.lae.wav" "$w/o.wav" full > /dev/null 2>&1; gd=$(sha256sum "$w/o.wav" 2>/dev/null | cut -c1-16 | tr a-f A-F)
    if [ "$ge" = "$enc" ]; then echo "encode $name: identical to MANIFEST (decode ${gd:-none} $( [ "$gd" = "$h24" ] && echo ok || { echo x >> "$w/bad"; echo DIFF; } ))"
    else echo "encode $name: DIFFERENT FILE ($ge vs $enc), decode ${gd:-none}"; echo x >> "$w/bad"; fi
    rm -f "$w/e.lae.wav"
done
[ -f "$w/bad" ] && bad=1; rm -rf "$w"; exit $bad
