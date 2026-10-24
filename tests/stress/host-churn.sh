#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Host side of the host/guest concurrency check: for SECONDS, keep changing
# files under DIR (a directory inside the shared folder) the way a developer
# editing a tree on the host would: rewrite a heartbeat file, create, append
# to, rename and delete files, make and remove directories.  The guest runs
# guest-watch.sh on the same directory and measures how long it takes to see
# each heartbeat.  Writes a short summary to stdout at the end.
#
# usage: host-churn.sh DIR SECONDS
DIR=${1:?dir}; SECS=${2:?seconds}
now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time * 1000' 2>/dev/null || echo $(( $(date +%s) * 1000 )); }
mkdir -p "$DIR/sub" || exit 1
end=$(( $(date +%s) + SECS ))
n=0; created=0; removed=0
while [ "$(date +%s)" -lt $end ]; do
	n=$((n + 1))
	# heartbeat: counter and host time (ms); written atomically via rename
	echo "$n $(now_ms)" > "$DIR/.hb.tmp" && mv "$DIR/.hb.tmp" "$DIR/hb.txt"
	# growing log the guest tails
	echo "line $n" >> "$DIR/append.log"
	# a file that changes size and content in place
	head -c $(( (n % 7 + 1) * 1000 )) /dev/zero | tr '\0' "$(printf '\\%03o' $((65 + n % 26)))" > "$DIR/inplace.dat"
	# churn: create a few, rename some, delete older ones
	i=0; while [ $i -lt 5 ]; do echo "$n-$i" > "$DIR/sub/f-$n-$i"; created=$((created + 1)); i=$((i + 1)); done
	[ -e "$DIR/sub/f-$n-0" ] && mv "$DIR/sub/f-$n-0" "$DIR/sub/r-$n"
	if [ $n -gt 3 ]; then
		old=$((n - 3)); rm -f "$DIR/sub/f-$old-"* "$DIR/sub/r-$old" && removed=$((removed + 1))
	fi
	mkdir -p "$DIR/d-$n" && [ $n -gt 2 ] && rmdir "$DIR/d-$((n - 2))" 2>/dev/null
	sleep 0.5
done
echo "host-churn: $n rounds, $created files created, dirs/renames/removes done; last heartbeat $n"
