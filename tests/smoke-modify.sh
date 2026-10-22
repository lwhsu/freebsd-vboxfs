#!/bin/sh
#
# SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Phase 2 smoke test for the modifying operations, run as root inside a
# guest with the module loaded and the share mounted.  Works in its own
# subdirectory of the mount point and removes it at the end.
#
# usage: smoke-modify.sh [mountpoint]
#
set -u
MNT=${1:-/mnt/share}
T=$MNT/smoke-modify
fail=0

ok() {		# command must succeed
	if "$@" >/dev/null 2>&1; then
		printf '    ok: %s\n' "$*"
	else
		printf '    FAIL (rc %d, expected 0): %s\n' "$?" "$*"
		fail=$((fail + 1))
	fi
}
nok() {		# command must fail
	if "$@" >/dev/null 2>&1; then
		printf '    FAIL (rc 0, expected failure): %s\n' "$*"
		fail=$((fail + 1))
	else
		printf '    ok (failed as expected, %s): %s\n' \
		    "$("$@" 2>&1 | tail -1)" "$*"
	fi
}
eq() {		# eq "what" expected actual
	if [ "$2" = "$3" ]; then
		printf '    ok: %s = %s\n' "$1" "$3"
	else
		printf '    FAIL: %s = %s, expected %s\n' "$1" "$3" "$2"
		fail=$((fail + 1))
	fi
}
mode() { stat -f %Lp "$1"; }
ino() { stat -f %i "$1"; }
size() { stat -f %z "$1"; }
mtime() { stat -f %m "$1"; }

echo "==> setup"
rm -rf "$T" 2>/dev/null
ok mkdir "$T"
cd "$T" || exit 1

echo "==> mkdir and modes"
ok mkdir d1
eq "mode d1" 755 "$(mode d1)"
ok mkdir -m 0700 d2
eq "mode d2 (mkdir -m 0700, host ignores mkdir modes)" 700 "$(mode d2)"
ok touch f1
eq "size f1" 0 "$(size f1)"
ok chmod 0640 f1
eq "mode f1 after chmod" 640 "$(mode f1)"
ok sh -c 'umask 077; : > f2'
eq "mode f2 (umask 077)" 600 "$(mode f2)"

echo "==> symlinks"
ok ln -s target lnk
eq "readlink lnk" target "$(readlink lnk)"
eq "mode lnk via lstat type" "$(stat -f %HT lnk)" "Symbolic Link"
ok ln -s f1 lnk2
eq "stat through symlink size" 0 "$(stat -L -f %z lnk2)"

echo "==> rename"
i=$(ino f1)
ok mv f1 d1/f2
nok test -e f1
ok test -f d1/f2
eq "ino preserved across rename" "$i" "$(ino d1/f2)"
ok mv d1 d3
ok test -f d3/f2
eq "ino of child after directory rename" "$i" "$(ino d3/f2)"
nok mv d3 d3/sub
ok touch g1 g2
j=$(ino g1)
ok mv g1 g2
eq "replace: g2 is the old g1" "$j" "$(ino g2)"
nok test -e g1
ok mkdir d4
ok mkdir d4/x
nok rmdir d4
ok rmdir d4/x
ok rmdir d4

echo "==> create exclusive / existing"
nok sh -c 'set -C; : > g2'
ok sh -c ': > g2'

echo "==> truncate and times"
ok truncate -s 1234 f3
eq "size after truncate -s 1234" 1234 "$(size f3)"
ok truncate -s 0 f3
eq "size after truncate -s 0" 0 "$(size f3)"
ok touch -d 2023-11-14T22:13:20Z f3
eq "mtime after touch -d" 1700000000 "$(mtime f3)"
eq "atime after touch -d" 1700000000 "$(stat -f %a f3)"
ok touch -d 1970-01-01T00:00:00Z f3
eq "mtime after touch -d epoch (sent as 1 ns)" 0 "$(mtime f3)"
ok touch -m -d 2024-01-01T00:00:00Z f3
eq "mtime after touch -m" 1704067200 "$(mtime f3)"
eq "atime unchanged by touch -m" 0 "$(stat -f %a f3)"
ok touch f3

echo "==> ownership"
ok chown 0:0 f3
nok chown 1000 f3
nok chgrp 1000 f3

echo "==> remove"
ok rm d3/f2
nok test -e d3/f2
ok rmdir d3
ok rm lnk lnk2 f2 f3 g2
ok rmdir d2
cd / && ok rmdir "$T"
nok test -e "$T"

echo "==> $fail failure(s)"
[ "$fail" -eq 0 ]
