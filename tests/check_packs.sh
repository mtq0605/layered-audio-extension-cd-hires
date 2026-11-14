#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# Extension packs (spec/lossy/SPEC.md, Section 5.6): one core, several mutually independent extensions. Checks on a synthetic
# 2-second clip that
#   1. without master2= the encoder writes the same file as before (the caller compares it with the conformance vectors);
#   2. with one further master and no silence added to the core, the core and the main body are unchanged (only the two length
#      fields and the bytes from the old CRC on differ);
#   3. a decoder without pack support (OLDDEC, any earlier v5 build) decodes the packed file like the plain one, and the new
#      decoder with pack=0 or without the argument gives the same bytes;
#   4. a pack's payload is, byte for byte, what "lae_enc <master> cd=<core> cdalign=0" writes, and pack=1 decodes like that file;
#   5. two further masters at fractional positions (an EQ variant, and the same content at half the rate): the core gets digital
#      silence once, both packs decode, a pack number beyond the list is refused;
#   6. the FLAC form carries the packs; the disc tool's split + repack keeps the file byte for byte.
# usage: check_packs.sh <bin folder with lae_enc, lae_dec, cd_variant, cut_wav, gen_suite, lae_cdx> <OLDDEC> <scratch folder>
# Runs under sh on Linux and under Git Bash on Windows (the programs may carry .exe). Exit code = number of failed checks.
set -u
BIN=$1; OLD=$2; W=$3; mkdir -p "$W"; bad=0
x() { if [ -x "$BIN/$1.exe" ]; then echo "$BIN/$1.exe"; else echo "$BIN/$1"; fi; }
ENC=$(x lae_enc); DEC=$(x lae_dec); VAR=$(x cd_variant); CUT=$(x cut_wav); GEN=$(x gen_suite); CDX=$(x lae_cdx)
ok() { echo "ok   $1"; }
no() { echo "FAIL $1"; bad=$((bad + 1)); }
same() { if cmp -s "$1" "$2"; then ok "$3"; else no "$3"; fi; }
# a decode into a fresh file: the output is removed first and a refused decode counts as a failure (FU33-3: a refused decode left the
# previous round's file in place, and the comparison then passed on it)
d() { rm -f -- "$3"; if "$@" >/dev/null 2>&1; then return 0; fi; no "decoder refused: $(basename "$1") $2 $3 ${4:-} ${5:-} ${6:-}"; return 1; }
cd "$W" || exit 99
[ -f electronic_352800.wav ] || "$GEN" . 352800 >/dev/null
[ -f electronic_176400.wav ] || "$GEN" . 176400 >/dev/null
"$CUT" electronic_352800.wav m.wav 5 2 >/dev/null; "$CUT" electronic_176400.wav m176.wav 5 2 >/dev/null
cp m.wav copy.wav; "$VAR" meq m.wav meq.wav high 8000 3 >/dev/null

"$ENC" m.wav a.wav 3 s1 >a.log 2>&1 || no "plain encode"
"$ENC" m.wav q.wav 3 s1 master2=copy.wav master2align=0 master2name=copy >q.log 2>&1 || no "encode with one further master"

# 2: the differing bytes are the RIFF size, the chunk size, and everything from the plain file's CRC on
offa=$(grep -abo "LAE " a.wav | tail -1 | cut -d: -f1); na=$(od -An -tu4 -j $((offa + 4)) -N4 a.wav | tr -d ' ')
crc=$(( offa + 8 + na - 4 + 1 ))   # 1-based position of the plain file's CRC (the chunk body's last 4 bytes)
first=$(cmp -l a.wav q.wav 2>/dev/null | awk -v s=$((offa + 8)) '$1 > s { print $1; exit }')   # the first difference after the chunk's size field
if [ -n "$first" ] && [ "$first" -ge "$crc" ]; then ok "core and main body unchanged"; else no "core and main body unchanged (first other difference at byte ${first:-none}, CRC at $crc)"; fi

# 3
for r in "full" "16" "cd24" "full 176400" "full 88200"; do
    d "$OLD" a.wav o1.wav $r; d "$OLD" q.wav o2.wav $r; d "$DEC" q.wav o3.wav $r pack=0; d "$DEC" q.wav o4.wav $r
    if cmp -s o1.wav o2.wav && cmp -s o1.wav o3.wav && cmp -s o1.wav o4.wav; then ok "pack 0 = earlier decoder [$r]"; else no "pack 0 = earlier decoder [$r]"; fi
done

# 4
"$VAR" extract q.wav core.wav >/dev/null; "$ENC" copy.wav s.wav 3 s1 cd=core.wav cdalign=0 >s.log 2>&1 || no "standalone encode against the core"
offp=$(grep -abo "LPAK" q.wav | head -1 | cut -d: -f1); offs=$(grep -abo "LAE " s.wav | tail -1 | cut -d: -f1)
n=$(od -An -tu4 -j $((offs + 4)) -N4 s.wav | tr -d ' '); pl=$((n - 4))
if cmp -s -n "$pl" -i "$((offp + 8 + 1 + 4)):$((offs + 8))" q.wav s.wav; then ok "pack 1 payload = standalone payload ($pl bytes)"; else no "pack 1 payload = standalone payload"; fi
for r in "full" "16" "cd24" "full 88200"; do d "$DEC" q.wav o5.wav $r pack=1; d "$OLD" s.wav o6.wav $r; same o5.wav o6.wav "pack 1 decodes like the standalone file [$r]"; done

# 5: positions from the encoder's own search
pos() { "$ENC" m.wav none.wav 3 s1 master2="$1" 2>&1 | sed -n 's/^ALIGN_SUGGEST whitened=aligned:\([^ ]*\) .*/\1/p' | head -1; }
p2=$(pos meq.wav); p3=$(pos m176.wav)
if [ -n "$p2" ] && [ -n "$p3" ] && [ ! -f none.wav ]; then ok "without a position: both searches run, nothing written ($p2, $p3)"; else no "search without a position"; fi
"$ENC" m.wav p.wav 3 s1 master2=meq.wav master2align="$p2" master2name="EQ" master3=m176.wav master3align="$p3" >p.log 2>&1 || no "encode with two further masters"
"$DEC" p.wav o7.wav full pack=1 >/dev/null && "$DEC" p.wav o8.wav full pack=2 >/dev/null && ok "packs 1 and 2 decode" || no "packs 1 and 2 decode"
if "$DEC" p.wav o9.wav full pack=3 >/dev/null 2>&1; then no "pack 3 refused"; else ok "pack 3 refused"; fi
d "$OLD" p.wav o10.wav full; d "$DEC" p.wav o11.wav full pack=0; same o10.wav o11.wav "two packs: pack 0 = earlier decoder"

# 6
"$ENC" m.wav qf.flac 3 s1 master2=copy.wav master2align=0 master2name=copy >qf.log 2>&1 || no "FLAC form encode"
for r in "full pack=0" "full pack=1" "16 pack=1"; do d "$DEC" q.wav o12.wav $r; d "$DEC" qf.flac o13.wav $r; same o12.wav o13.wav "FLAC form = WAV form [$r]"; done
"$CDX" split q.wav q_base.wav q_ext.lax track=1 >/dev/null && "$CDX" repack q_base.wav q_ext.lax q_back.wav >/dev/null; same q.wav q_back.wav "disc tool: split + repack keeps the file"

echo "extension packs: $bad check(s) failed"
exit $bad
