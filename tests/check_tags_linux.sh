#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# LAEY tags on Linux (2026-11-06): encodes a 2-s conformance source clip with tags (ASCII and UTF-8 Chinese), a cover and a back
# picture, and compares the file with the Windows MSVC build (x64 and x86 gave the same bytes). Then lae_enc tag replaces the tags and
# removes them again: the result must equal the file encoded without tags, byte for byte.
set -u
cd "$(dirname "$0")/.."
w=$(mktemp -d); bad=0; F="-O2 -std=c++17 -ffp-contract=off -D_USE_MATH_DEFINES -pthread"
REF_PLAIN=27a49c088884e29727320bf122b9b72eeaaada0569006473aad101f7cebd65f3
REF_TAGGED=1fef3fbec28512b20811d0f58def5cc2322f3e2a5349cb019bcddac1a0842846
for p in lae_enc gen_suite cut_wav; do g++ $F -o "$w/$p" src/$p.cpp 2> "$w/b_$p.txt" || { echo "build $p: FAILED"; head -20 "$w/b_$p.txt"; exit 1; }; done
"$w/gen_suite" "$w" 88200 > /dev/null && "$w/cut_wav" "$w/sweep_88200.wav" "$w/s.wav" 5 2 > /dev/null || { echo "source: FAILED"; exit 1; }
# two tiny "pictures": a PNG header with IHDR (1 x 1, 8-bit RGB) and a JPEG start with an SOF0 marker; stored byte for byte
printf '\211PNG\r\n\032\n\000\000\000\015IHDR\000\000\000\001\000\000\000\001\010\002\000\000\000\220wS\336' > "$w/c.png"
printf '\377\330\377\300\000\021\010\000\002\000\003\003\001\021\000\002\021\001\003\021\001\377\331' > "$w/b.jpg"
t1=$(printf 'TITLE=\345\244\247\350\260\203 A1')   # UTF-8 "大调 A1"
"$w/lae_enc" "$w/s.wav" "$w/plain.wav" 3 > "$w/l1.txt" 2>&1 || { echo "encode without tags: FAILED"; tail -3 "$w/l1.txt"; bad=1; }
"$w/lae_enc" "$w/s.wav" "$w/tagged.wav" 3 "--tag=$t1" --tag=ARTIST=mtq0605 tag=COMMENT=test "--cover=$w/c.png" "picture=4:$w/b.jpg" > "$w/l2.txt" 2>&1 || { echo "encode with tags: FAILED"; tail -3 "$w/l2.txt"; bad=1; }
gp=$(sha256sum "$w/plain.wav" | cut -c1-64); gt=$(sha256sum "$w/tagged.wav" | cut -c1-64)
[ "$gp" = "$REF_PLAIN" ] && echo "tags: file without tags identical" || { echo "tags: file without tags DIFFERENT ($gp)"; bad=1; }
[ "$gt" = "$REF_TAGGED" ] && echo "tags: file with tags identical" || { echo "tags: file with tags DIFFERENT ($gt)"; bad=1; }
"$w/lae_enc" tag "$w/tagged.wav" "$w/re.wav" --tag=TITLE=other > "$w/l3.txt" 2>&1 || { echo "lae_enc tag: FAILED"; cat "$w/l3.txt"; bad=1; }
"$w/lae_enc" tag "$w/re.wav" "$w/none.wav" > "$w/l4.txt" 2>&1 || { echo "lae_enc tag (remove): FAILED"; cat "$w/l4.txt"; bad=1; }
cmp -s "$w/none.wav" "$w/plain.wav" && echo "tags: replaced, then removed: identical to the file without tags" || { echo "tags: after removing the tags the file DIFFERS from the file without tags"; bad=1; }
rm -rf "$w"; exit $bad
