#!/bin/sh
#
# SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Phase 1 smoke test, run as root inside a guest: load the module, mount
# the share, browse it read-only, unmount, unload.  Every step that fails
# is reported and the script exits non-zero at the end.
#
# usage: smoke-browse.sh [-k vboxvfs.ko] [-m mount_vboxvfs] [share] [mountpoint]
#
set -u
KO=./vboxvfs.ko
MOUNT=mount_vboxvfs
while getopts k:m: opt; do
	case $opt in
	k) KO=$OPTARG ;;
	m) MOUNT=$OPTARG ;;
	*) echo "usage: $0 [-k vboxvfs.ko] [-m mount_vboxvfs] [share] [mountpoint]" >&2; exit 64 ;;
	esac
done
shift $((OPTIND - 1))
SHARE=${1:-share}
MNT=${2:-/mnt/share}
fail=0

step() {
	printf '==> %s\n' "$*"
}
check() {
	if "$@"; then
		printf '    ok: %s\n' "$*"
	else
		printf '    FAIL (%d): %s\n' "$?" "$*"
		fail=$((fail + 1))
	fi
}

step "kernel"
uname -a
sysctl kern.osreldate

step "load modules"
kldstat -q -n vboxguest || check kldload vboxguest
kldstat -q -n vboxvfs && check kldunload vboxvfs
check kldload "$KO"
if ! kldstat -q -n vboxvfs; then
	echo "    vboxvfs is not loaded; stopping here so that nmount does not"
	echo "    auto-load another vboxvfs.ko from /boot/modules"
	exit 1
fi
dmesg | tail -3

step "mount"
mkdir -p "$MNT"
check "$MOUNT" -o uid=0,gid=0 "$SHARE" "$MNT"
check mount -p
check df -h "$MNT"
check sysctl vfs.vboxvfs

step "browse"
check ls -la "$MNT"
check stat "$MNT"
check stat "$MNT/."
(cd "$MNT" && check pwd && check ls -la . && check sh -c 'cd . && pwd') || fail=$((fail + 1))
for d in "$MNT"/*/; do
	[ -d "$d" ] || continue
	check sh -c "cd '$d' && pwd && cd .. && pwd"
	check ls -la "$d"
	break
done
check find "$MNT" -maxdepth 2 -print
check sh -c "ls -lai '$MNT' | head -5"
check sh -c "stat -f '%i %N' '$MNT'/* | head -5"

step "name cache / repeated stat"
check sh -c "for i in 1 2 3; do stat -f '%i %m %z %N' '$MNT' >/dev/null; done"

step "unmount and unload"
check umount "$MNT"
check kldunload vboxvfs
dmesg | tail -3
vmstat -m | grep -i vboxvfs || echo "    (no vboxvfs malloc type left)"

echo "==> $fail failure(s)"
[ "$fail" -eq 0 ]
