#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Group the "only FreeBSD new driver" failures (from cmp-new-vs-linux.txt) by
# operation / expected / got, with the test description and example lines.
# usage: classify-only-new.sh NEW_DIR  -> writes only-new-classified.txt
set -eu
NEW=$1
cd "$NEW"
awk '/^== only FreeBSD new driver ==/ { f = 1; next } /^== only Linux ==/ { f = 0 } f && NF == 2 { print $1 "\t" $2 }' cmp-new-vs-linux.txt | sort > /tmp/onlynew.$$
# join with failures.tsv (file, n, op, expected, got)
sort failures.tsv > /tmp/ft.$$
join -t "$(printf '\t')" -j 1 <(awk -F'\t' '{ print $1 "/" $2 "\t" $0 }' /tmp/onlynew.$$ | cut -f1 | sort) <(awk -F'\t' '{ print $1 "/" $2 "\t" $3 "\t" $4 "\t" $5 }' /tmp/ft.$$ | sort) > /tmp/joined.$$ 2>/dev/null || true
{
	echo "# only-FreeBSD (new driver) failures grouped by operation / expected / got"
	echo "# total: $(wc -l < /tmp/onlynew.$$ | tr -d ' ')"
	echo
	awk -F'\t' '{ c[$2 "\t" $3 "\t" $4]++ } END { for (k in c) printf("%6d\t%s\n", c[k], k) }' /tmp/joined.$$ | sort -rn
	echo
	echo "# examples per group (test file, assertion, desc, raw line)"
	awk -F'\t' '{ k = $2 "\t" $3 "\t" $4; if (n[k] < 3) { n[k]++; print k "\t" $1 } }' /tmp/joined.$$ | sort | while IFS="$(printf '\t')" read -r op ex got key; do
		t=${key%/*}; a=${key##*/}
		desc=$(sed -n 's/^desc="\(.*\)"/\1/p' ${PJDFSTEST:-/home/lwhsu/vboxfs/pjdfstest}/tests/$t)
		line=$(/usr/bin/grep -E "^not ok $a( |$)" "log/$t.log" | head -1 | cut -c1-200)
		printf '%s | %s | %s\n    %s #%s (%s)\n    %s\n' "$op" "$ex" "$got" "$t" "$a" "$desc" "$line"
	done
} > only-new-classified.txt
rm -f /tmp/onlynew.$$ /tmp/ft.$$ /tmp/joined.$$
head -40 only-new-classified.txt
