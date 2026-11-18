#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# lae_repair on Linux (2026-11-19). Builds lae_udec and lae_repair with g++ and clang++ (lae_enc, laes, gen_suite and cut_wav once,
# with g++), makes a 5-second clip as LAEY and LAES, each in the WAV form and the FLAC form, and checks with every build:
#   1. a file in the LAE form is "nothing to repair": exit 1, nothing written;
#   2. the FLAC form re-encoded by `flac` (inter-channel decorrelation) is refused by `lae_udec verify` (exit 3); `lae_repair` repairs
#      it (exit 0), the result passes `lae_udec verify`, and its STREAMINFO and audio frames are byte-identical to the original's;
#   3. the WAV form packed by `flac --keep-foreign-metadata` ("riff" blocks): `lae_repair` gives back the original WAV file, byte for byte.
# Checks 2 and 3 need the `flac` command (FLAC 1.3 or later; FLAC=<path> chooses another); without it they are skipped and said so.
# usage: sh tests/check_repair_linux.sh
set -u
cd "$(dirname "$0")/.."
w=$(mktemp -d); bad=0; F="-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread"
FL=${FLAC:-flac}; command -v "$FL" > /dev/null 2>&1 && hasflac=1 || hasflac=0
# the bytes after the last metadata block (the audio frames) and the 34 bytes of STREAMINFO
frames() { perl -e 'open F,"<:raw",$ARGV[0] or die; local $/; $b=<F>; $p=4; while(1){$h=ord(substr($b,$p,1)); $n=unpack("N","\0".substr($b,$p+1,3)); $p+=4+$n; last if $h&0x80} print substr($b,$p);' "$1"; }
si() { perl -e 'open F,"<:raw",$ARGV[0] or die; local $/; $b=<F>; print unpack("H*",substr($b,8,34));' "$1"; }
for p in lae_enc gen_suite cut_wav; do g++ $F -o "$w/$p" src/$p.cpp 2> "$w/b.txt" || { echo "build $p: FAILED"; head -20 "$w/b.txt"; rm -rf "$w"; exit 1; }; done
g++ $F -o "$w/laes" src/laes_tool.cpp 2> "$w/b.txt" || { echo "build laes: FAILED"; head -20 "$w/b.txt"; rm -rf "$w"; exit 1; }
"$w/gen_suite" "$w" 176400 20 transients > /dev/null; "$w/cut_wav" "$w/transients_176400.wav" "$w/m.wav" 5 2 > /dev/null
"$w/lae_enc" "$w/m.wav" "$w/y_wav.wav" 3 s1 > "$w/e.txt" 2>&1 || { echo "lae_enc WAV form: FAILED"; tail -3 "$w/e.txt"; bad=1; }
"$w/lae_enc" "$w/m.wav" "$w/y_flac.flac" 3 s1 > "$w/e.txt" 2>&1 || { echo "lae_enc FLAC form: FAILED"; tail -3 "$w/e.txt"; bad=1; }
"$w/laes" enc "$w/s_flac.flac" --derive-core "$w/m.wav" > "$w/e.txt" 2>&1 || { echo "laes enc FLAC form: FAILED"; tail -3 "$w/e.txt"; bad=1; }
"$w/laes" enc "$w/s_wav.wav" --derive-core "$w/m.wav" > "$w/e.txt" 2>&1 || { echo "laes enc WAV form: FAILED"; tail -3 "$w/e.txt"; bad=1; }
if [ $hasflac = 1 ]; then echo "flac: $("$FL" --version 2>&1 | head -1)"
    for f in y_flac s_flac; do "$FL" -f -s -o "$w/re_$f.flac" "$w/$f.flac" 2> "$w/f.txt" || { echo "flac re-encode $f: FAILED"; tail -3 "$w/f.txt"; bad=1; }; done
    for f in y_wav s_wav; do "$FL" -f -s --keep-foreign-metadata -o "$w/riff_$f.flac" "$w/$f.wav" 2> "$w/f.txt" || { echo "flac --keep-foreign-metadata $f: FAILED"; tail -3 "$w/f.txt"; bad=1; }; done
else echo "flac not found: checks 2 and 3 skipped (install FLAC, or give FLAC=<path>)"; fi
for cc in g++ clang++; do
    $cc $F -DLAE_STATIC -o "$w/lae_udec" src/lae_udec.cpp src/lae_any.cpp src/lae_api.cpp src/laes_api.cpp 2> "$w/b.txt" || { echo "$cc build lae_udec: FAILED"; head -20 "$w/b.txt"; bad=1; continue; }
    $cc $F -DLAE_STATIC -o "$w/lae_repair" src/lae_repair.cpp src/lae_any.cpp src/lae_api.cpp src/laes_api.cpp 2> "$w/b.txt" || { echo "$cc build lae_repair: FAILED"; head -20 "$w/b.txt"; bad=1; continue; }
    for f in y_wav.wav y_flac.flac s_flac.flac s_wav.wav; do rm -f "$w/out"
        "$w/lae_repair" "$w/$f" "$w/out" > "$w/r.txt" 2>&1; r=$?
        [ $r -eq 1 ] && [ ! -e "$w/out" ] && [ ! -e "$w/out.part" ] && echo "$cc: $f in the LAE form: nothing to repair" || { echo "$cc: $f: FAILED (exit $r)"; tail -3 "$w/r.txt"; bad=1; }
    done
    [ $hasflac = 1 ] || continue
    for f in y_flac s_flac; do rm -f "$w/out.flac"
        "$w/lae_udec" verify "$w/re_$f.flac" > "$w/v.txt" 2>&1; v0=$?
        "$w/lae_repair" "$w/re_$f.flac" "$w/out.flac" > "$w/r.txt" 2>&1; r=$?
        "$w/lae_udec" verify "$w/out.flac" > "$w/v.txt" 2>&1; v1=$?
        frames "$w/out.flac" > "$w/a.bin" 2> /dev/null; frames "$w/$f.flac" > "$w/b.bin"
        if [ $v0 -eq 3 ] && [ $r -eq 0 ] && [ $v1 -eq 0 ] && cmp -s "$w/a.bin" "$w/b.bin" && [ "$(si "$w/out.flac")" = "$(si "$w/$f.flac")" ]; then
            echo "$cc: $f re-encoded by flac: refused before (exit 3), repaired, verify passed, frames and STREAMINFO identical"
        else echo "$cc: $f re-encoded by flac: FAILED (verify before $v0, repair $r, verify after $v1)"; tail -3 "$w/r.txt"; bad=1; fi
    done
    for f in y_wav s_wav; do rm -f "$w/out.wav"
        "$w/lae_repair" "$w/riff_$f.flac" "$w/out.wav" > "$w/r.txt" 2>&1; r=$?
        if [ $r -eq 0 ] && cmp -s "$w/out.wav" "$w/$f.wav"; then echo "$cc: $f packed by flac --keep-foreign-metadata: original WAV file given back, byte for byte"
        else echo "$cc: $f packed by flac --keep-foreign-metadata: FAILED (repair $r)"; tail -3 "$w/r.txt"; bad=1; fi
    done
done
rm -rf "$w"; [ $bad -eq 0 ] && echo "lae_repair: all passed" || { echo "lae_repair: FAILED"; exit 1; }
