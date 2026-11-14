#!/bin/sh
# SPDX-FileCopyrightText: 2026 mtq0605
# SPDX-License-Identifier: Apache-2.0
# Checks that the Java MDCT tables (java/src/com/lae/laey/MdctTables.java) hold, table by table, exactly the literals of the normative
# C++ tables (src/mdct_tables.h): same tables, same number of values, same text of every value (hexadecimal literals are exact in both
# languages, so equal text means equal doubles). A table changed on one side only would make the Java decoder differ from the C++.
# usage: check_mdct_tables.sh [repository root, default: the folder above this script]. Exit code = number of differing tables.
# Runs in sh (Linux, macOS, Git Bash on Windows); needs grep and sed.
R=${1:-$(cd "$(dirname "$0")/.." && pwd)}; H="$R/src/mdct_tables.h"; J="$R/java/src/com/lae/laey/MdctTables.java"; bad=0
[ -f "$H" ] && [ -f "$J" ] || { echo "FAIL: $H or $J not found"; exit 99; }
names=$(sed -n 's/^static const double \([A-Z_]*\)\[.*/\1/p' "$H"); jn=$(sed -n 's/^ *static final double\[\] \([A-Z_]*\) = .*/\1/p' "$J")
[ -n "$names" ] && [ "$(echo $names)" = "$(echo $jn)" ] || { echo "FAIL: the tables differ: C++ [$(echo $names)], Java [$(echo $jn)]"; exit 99; }
for t in $names; do
    c=$(sed -n "s/^static const double $t\[[0-9]*\] = {\(.*\)};.*/\1/p" "$H"); j=$(sed -n "s/^ *static final double\[\] $t = {\(.*\)};.*/\1/p" "$J")
    n=$(printf '%s' "$c" | tr ',' '\n' | grep -c .); d=$(sed -n "s/^static const double $t\[\([0-9]*\)\].*/\1/p" "$H")
    if [ -n "$c" ] && [ "$c" = "$j" ] && [ "$n" = "$d" ]; then echo "ok   $t ($n values)"; else echo "FAIL $t (C++ $n of $d values; the Java literals differ)"; bad=$((bad + 1)); fi
done
echo "MDCT tables: $bad table(s) differ"; exit $bad
