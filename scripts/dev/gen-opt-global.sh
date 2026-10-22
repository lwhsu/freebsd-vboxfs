#!/bin/sh
#
# SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Write an opt_global.h matching the running kernel, from kern.conftxt
# and the kernel's option tables, so that a module built out of tree sees
# the same kernel option set (INVARIANTS, BUF_TRACKING, ... change struct
# layouts and assertions).  Use the output directory as KERNBUILDDIR.
#
# usage: gen-opt-global.sh [outdir] [sysdir]
#
set -eu
OUT=${1:-/root/kernopts}
SYSDIR=${2:-/usr/src/sys}
ARCH=$(uname -m)
mkdir -p "$OUT"
# Options that go to opt_global.h: the first column of entries whose second
# column is empty (default opt_global.h) or opt_global.h, in both tables.
cat "$SYSDIR/conf/options" "$SYSDIR/conf/options.$ARCH" 2>/dev/null |
    sed -e 's/#.*//' | awk 'NF >= 1 && ($2 == "" || $2 == "opt_global.h") { print $1 }' |
    sort -u > "$OUT/.global-options"
sysctl -n kern.conftxt | awk '$1 == "options" { print $2 }' | while read -r opt; do
	name=${opt%%=*}
	if grep -qx "$name" "$OUT/.global-options"; then
		case $opt in
		*=*)	printf '#define %s %s\n' "$name" "${opt#*=}" ;;
		*)	printf '#define %s 1\n' "$name" ;;
		esac
	fi
done | sort > "$OUT/opt_global.h"
# The build also wants the per-subsystem opt_*.h to exist; empty is fine.
for h in opt_ddb.h opt_witness.h opt_vm.h opt_kern_tls.h opt_inet.h opt_inet6.h; do
	[ -f "$OUT/$h" ] || : > "$OUT/$h"
done
echo "$OUT/opt_global.h: $(wc -l < "$OUT/opt_global.h") options"
grep -E 'INVARIANTS|WITNESS|BUF_TRACKING|DEBUG_VFS_LOCKS' "$OUT/opt_global.h" || true
