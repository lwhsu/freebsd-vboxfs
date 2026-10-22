#!/bin/sh
# Compare three pjdfstest runs: NEW (this dir), OLD driver, LINUX control.
# usage: compare.sh NEW_DIR OLD_DIR LINUX_DIR
#   NEW_DIR/failures.tsv and OLD_DIR/failures.tsv are produced by
#   pjd-failures.sh ("<dir>/<file>.t<TAB><assertion>..."); LINUX_DIR has
#   failures.txt ("<dir>/<file>.t <assertion>").  Writes into NEW_DIR:
#   failures.txt, cmp-new-vs-linux.txt, cmp-new-vs-old.txt, per-dir-3way.md
set -eu
NEW=$1; OLD=$2; LNX=$3
cd "$NEW"
awk -F'\t' '{ print $1 " " $2 }' failures.tsv | sort -u > failures.txt
awk -F'\t' '{ print $1 " " $2 }' "$OLD/failures.tsv" | sort -u > /tmp/old-failures.$$
sort -u "$LNX/failures.txt" > /tmp/lnx-failures.$$
comm -3 failures.txt /tmp/lnx-failures.$$ > /dev/null  # sanity
{
	echo "# new driver vs Linux vboxsf (same pjdfstest 85a8aea)"
	echo "only-new: $(comm -23 failures.txt /tmp/lnx-failures.$$ | wc -l | tr -d ' ')  only-linux: $(comm -13 failures.txt /tmp/lnx-failures.$$ | wc -l | tr -d ' ')  both: $(comm -12 failures.txt /tmp/lnx-failures.$$ | wc -l | tr -d ' ')"
	echo; echo "== only FreeBSD new driver =="; comm -23 failures.txt /tmp/lnx-failures.$$
	echo; echo "== only Linux =="; comm -13 failures.txt /tmp/lnx-failures.$$
} > cmp-new-vs-linux.txt
{
	echo "# new driver vs old driver"
	echo "only-new: $(comm -23 failures.txt /tmp/old-failures.$$ | wc -l | tr -d ' ')  only-old: $(comm -13 failures.txt /tmp/old-failures.$$ | wc -l | tr -d ' ')  both: $(comm -12 failures.txt /tmp/old-failures.$$ | wc -l | tr -d ' ')"
	echo; echo "== only new (regressions vs old) =="; comm -23 failures.txt /tmp/old-failures.$$
} > cmp-new-vs-old.txt
# per-directory 3-way table: files with failures / failed assertions
perdir() {	# $1 = failures file (space separated)
	awk '{ split($1, a, "/"); d[a[1]]++; f[$1] = 1 } END { for (k in f) { split(k, a, "/"); nf[a[1]]++ } for (k in d) print k, d[k], nf[k] }' "$1"
}
perdir failures.txt | sort > /tmp/pd-new.$$
perdir /tmp/old-failures.$$ | sort > /tmp/pd-old.$$
perdir /tmp/lnx-failures.$$ | sort > /tmp/pd-lnx.$$
{
	echo "| directory | new: failed assertions (files) | old driver | Linux vboxsf |"
	echo "|---|---|---|---|"
	for d in chflags chmod chown ftruncate granular link mkdir mkfifo mknod open posix_fallocate rename rmdir symlink truncate unlink utimensat; do
		n=$(awk -v d=$d '$1 == d { printf("%d (%d)", $2, $3) }' /tmp/pd-new.$$); o=$(awk -v d=$d '$1 == d { printf("%d (%d)", $2, $3) }' /tmp/pd-old.$$); l=$(awk -v d=$d '$1 == d { printf("%d (%d)", $2, $3) }' /tmp/pd-lnx.$$)
		echo "| $d | ${n:-0} | ${o:-0} | ${l:-0} |"
	done
	echo "| **total** | $(wc -l < failures.txt | tr -d ' ') | $(wc -l < /tmp/old-failures.$$ | tr -d ' ') | $(wc -l < /tmp/lnx-failures.$$ | tr -d ' ') |"
} > per-dir-3way.md
rm -f /tmp/old-failures.$$ /tmp/lnx-failures.$$ /tmp/pd-*.$$
cat per-dir-3way.md; echo; head -1 cmp-new-vs-linux.txt cmp-new-vs-old.txt | tail -1; sed -n 2p cmp-new-vs-linux.txt; sed -n 2p cmp-new-vs-old.txt
