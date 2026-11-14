#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# LAEY FLAC form (spec/lossy/SPEC.md, Section 3.4): encodes one master excerpt in the WAV form and in the FLAC form and checks that
#   * every route decodes to the same bytes from both forms; .lae = the WAV form (the default), .lae --form=flac = the FLAC form, byte for byte;
#   * the FLAC file's "LAE1" data equals the WAV file's "LAE " chunk body (cmp of the extracted bytes);
#   * a smaller piece size (test hook LAEY_TEST_PIECE) gives several LAE1 blocks and the same decode;
#   * tags: encoding with tags equals tagging afterwards; removing the tags gives the untagged file; the form cannot be changed by tag;
#   * damaged files are refused with exit code 3, a plain FLAC and a wrong --form with exit code 1.
# usage: check_flacform.sh <lae_enc> <lae_dec> <master.wav (10 s is enough)> <scratch folder> [mode, default 3]
# Runs in sh (Linux, macOS, Git Bash on Windows); needs cmp, dd, od, printf.
ENC=$1; DEC=$2; M=$3; W=$4; MODE=${5:-3}
[ -n "$W" ] || { echo "usage: check_flacform.sh <lae_enc> <lae_dec> <master.wav> <scratch folder> [mode]"; exit 1; }
mkdir -p "$W" || exit 1
ok=0; bad=0
pass() { ok=$((ok + 1)); }
fail() { bad=$((bad + 1)); echo "FAIL: $1"; }
same() { if cmp -s "$1" "$2"; then pass; else fail "$3"; fi; }
code() { "$@" > "$W/last.txt" 2>&1; echo $?; }
be24() { od -An -tu1 -j "$2" -N 3 "$1" | { read a b c; echo $(( (a << 16) | (b << 8) | c )); }; }

"$ENC" "$M" "$W/a.wav" "$MODE" > "$W/enc_wav.txt" 2>&1 || fail "WAV form encode"
"$ENC" "$M" "$W/a.flac" "$MODE" > "$W/enc_flac.txt" 2>&1 || fail "FLAC form encode"
"$ENC" "$M" "$W/b.lae" "$MODE" > /dev/null 2>&1 || fail ".lae encode"
"$ENC" "$M" "$W/c.lae" "$MODE" --form=flac > /dev/null 2>&1 || fail ".lae --form=flac encode"
same "$W/b.lae" "$W/a.wav" ".lae is not the WAV form (the default)"
same "$W/c.lae" "$W/a.flac" ".lae --form=flac is not the FLAC form"
[ "$(code "$ENC" "$M" "$W/x.wav" "$MODE" --form=flac)" = 1 ] && pass || fail "--form=flac with a .wav name was not refused"
[ ! -e "$W/x.wav" ] && pass || fail "a refused run wrote a file"

rate=$(od -An -tu4 -j 24 -N 4 "$W/a.wav" | tr -d ' ')   # the base rate (fmt chunk of the encoder's WAV: offset 24)
for r in "full" "16" "cd24" "full $((rate * 2))"; do
    "$DEC" "$W/a.wav" "$W/w.out" $r > /dev/null 2>&1 || fail "decode WAV form, route $r"
    "$DEC" "$W/a.flac" "$W/f.out" $r > /dev/null 2>&1 || fail "decode FLAC form, route $r"
    same "$W/w.out" "$W/f.out" "route $r differs between the forms"
done
"$DEC" "$W/a.wav" "$W/ref.out" full > /dev/null 2>&1

# the extension: one APPLICATION block (type 2, last) right after STREAMINFO in an untagged file written by lae_enc
if [ "$(od -An -tu1 -j 42 -N 1 "$W/a.flac" | tr -d ' ')" = 130 ]; then
    n=$(be24 "$W/a.flac" 43)
    dd if="$W/a.flac" of="$W/ext.bin" bs=1 skip=50 count=$((n - 4)) 2> /dev/null
    laeoff=$(( $(wc -c < "$W/a.wav") - (n - 4) - ((n - 4) % 2) ))
    dd if="$W/a.wav" of="$W/chunk.bin" bs=1 skip="$laeoff" count=$((n - 4)) 2> /dev/null
    same "$W/ext.bin" "$W/chunk.bin" "the LAE1 data differs from the LAE chunk body"
else echo "note: the extension is in more than one block; the byte comparison with the LAE chunk is skipped"; fi

LAEY_TEST_PIECE=300000 "$ENC" "$M" "$W/p.flac" "$MODE" > "$W/enc_piece.txt" 2>&1 || fail "encode with small pieces"
grep -q "extension in 1 LAE1 block" "$W/enc_piece.txt" && fail "the small-piece file has one block" || pass
"$DEC" "$W/p.flac" "$W/p.out" full > /dev/null 2>&1; same "$W/p.out" "$W/ref.out" "the multi-block file decodes differently"

"$ENC" "$M" "$W/t1.flac" "$MODE" --tag=TITLE=Test --tag=ARTIST=lae > /dev/null 2>&1 || fail "encode with tags"
"$ENC" tag "$W/a.flac" "$W/t2.flac" --tag=TITLE=Test --tag=ARTIST=lae > /dev/null 2>&1 || fail "lae_enc tag (FLAC form)"
same "$W/t1.flac" "$W/t2.flac" "encoding with tags differs from tagging afterwards"
"$ENC" tag "$W/t1.flac" "$W/t3.flac" > /dev/null 2>&1 || fail "lae_enc tag without tags"
same "$W/t3.flac" "$W/a.flac" "removing the tags does not give the untagged file"
"$ENC" tag "$W/p.flac" "$W/t4.flac" --tag=TITLE=Test > /dev/null 2>&1 || fail "lae_enc tag on a multi-block file"
"$DEC" "$W/t4.flac" "$W/t4.out" full > /dev/null 2>&1; same "$W/t4.out" "$W/ref.out" "the tagged multi-block file decodes differently"
[ "$(code "$ENC" tag "$W/a.flac" "$W/t5.wav" --tag=TITLE=x)" = 1 ] && pass || fail "tag: FLAC form into a .wav name was not refused"
[ "$(code "$ENC" tag "$W/a.wav" "$W/t5.flac" --tag=TITLE=x)" = 1 ] && pass || fail "tag: WAV form into a .flac name was not refused"

size=$(wc -c < "$W/a.flac"); audio=$((50 + $(be24 "$W/a.flac" 43) - 4))
damage() { cp "$W/a.flac" "$W/d.flac"; printf '\125' | dd of="$W/d.flac" bs=1 seek="$1" conv=notrunc 2> /dev/null; }
refused() { rm -f "$W/d.out"; c=$(code "$DEC" "$W/d.flac" "$W/d.out" full); if [ "$c" = "$1" ] && [ ! -e "$W/d.out" ]; then pass; else fail "$2 (exit $c)"; fi; }
if [ "$(od -An -tu1 -j 42 -N 1 "$W/a.flac" | tr -d ' ')" = 130 ]; then
    damage $((audio + (size - audio) / 2)); refused 3 "a damaged audio frame was not refused"
    damage $((50 + (audio - 50) / 2)); refused 3 "a damaged extension was not refused"
    damage 30; refused 3 "a wrong STREAMINFO MD5 was not refused"
    cp "$W/a.flac" "$W/d.flac"; printf 'LAE2' | dd of="$W/d.flac" bs=1 seek=46 conv=notrunc 2> /dev/null; refused 1 "an LAE2 block was not reported as LAES"
    cp "$W/a.flac" "$W/d.flac"; printf 'XXXX' | dd of="$W/d.flac" bs=1 seek=46 conv=notrunc 2> /dev/null; refused 1 "a FLAC file without LAE1 blocks was not reported as plain FLAC"
elif [ "$(od -An -tu1 -j 42 -N 1 "$W/a.flac" | tr -d ' ')" = 2 ]; then echo "note: the extension is in more than one block; the five damage checks are skipped"
else fail "the block after STREAMINFO is not the LAE1 block (type byte $(od -An -tu1 -j 42 -N 1 "$W/a.flac" | tr -d ' ')): the five damage checks were not run"; fi   # FU33-4: not skipped silently
echo "summary: $ok ok, $bad failed"
exit $bad
