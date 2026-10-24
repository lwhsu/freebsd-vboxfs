#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Phase 3 smoke test for the data path, run as root inside a guest with
# the module loaded and the share mounted: read/write through the buffer
# cache, appends, partial blocks, holes, a tree copy, and read/write/mmap
# coherence with a small C program.  Works in its own subdirectory of
# the mount point and removes it at the end.
#
# usage: smoke-io.sh [mountpoint]
#
set -u
MNT=${1:-/mnt/share}
if ! mount -p | awk -v m="$MNT" '$2 == m && $3 == "vboxvfs" { found = 1 } END { exit !found }'; then
	echo "$0: $MNT is not a mounted vboxvfs file system; refusing to run" >&2
	exit 2
fi
T=$MNT/smoke-io
L=/tmp/smoke-io.$$
fail=0

ok() {
	if "$@" >/dev/null 2>&1; then
		printf '    ok: %s\n' "$*"
	else
		printf '    FAIL (rc %d): %s\n' "$?" "$*"
		fail=$((fail + 1))
	fi
}
eq() {
	if [ "$2" = "$3" ]; then
		printf '    ok: %s = %s\n' "$1" "$3"
	else
		printf '    FAIL: %s = %s, expected %s\n' "$1" "$3" "$2"
		fail=$((fail + 1))
	fi
}
sum() { md5 -q "$1"; }

echo "==> setup"
rm -rf "$T" "$L"; mkdir -p "$T" "$L" || exit 1
dd if=/dev/urandom of="$L/rand1m" bs=1m count=1 2>/dev/null
dd if=/dev/urandom of="$L/rand100k" bs=1k count=100 2>/dev/null
dd if=/dev/urandom of="$L/rand3" bs=1 count=3 2>/dev/null

echo "==> whole-file copies"
ok cp "$L/rand1m" "$T/rand1m"
eq "size rand1m" 1048576 "$(stat -f %z "$T/rand1m")"
eq "md5 rand1m (cached read)" "$(sum "$L/rand1m")" "$(sum "$T/rand1m")"
ok cp "$L/rand100k" "$T/rand100k"
eq "md5 rand100k" "$(sum "$L/rand100k")" "$(sum "$T/rand100k")"
ok cp "$L/rand3" "$T/rand3"
eq "md5 rand3" "$(sum "$L/rand3")" "$(sum "$T/rand3")"
ok cp "$T/rand1m" "$L/rand1m.back"
eq "md5 copied back" "$(sum "$L/rand1m")" "$(sum "$L/rand1m.back")"

echo "==> small writes, appends, overwrite in the middle"
: > "$T/small"
i=0; while [ $i -lt 100 ]; do printf 'line %03d\n' $i >> "$T/small"; i=$((i + 1)); done
eq "size after 100 appends" 900 "$(stat -f %z "$T/small")"
eq "last line" "line 099" "$(tail -1 "$T/small")"
printf 'XXXXXXXX' | dd of="$T/small" bs=1 seek=450 conv=notrunc 2>/dev/null
eq "overwrite in the middle" "XXXXXXXX" "$(dd if="$T/small" bs=1 skip=450 count=8 2>/dev/null)"
eq "size unchanged by overwrite" 900 "$(stat -f %z "$T/small")"
printf 'tail' >> "$T/small"
eq "size after append" 904 "$(stat -f %z "$T/small")"

echo "==> holes and truncation"
dd if="$L/rand3" of="$T/sparse" bs=1 seek=200000 2>/dev/null
eq "size of sparse file" 200003 "$(stat -f %z "$T/sparse")"
eq "hole reads as zeros" "$(dd if=/dev/zero bs=1 count=1000 2>/dev/null | md5 -q)" "$(dd if="$T/sparse" bs=1 count=1000 2>/dev/null | md5 -q)"
eq "data after the hole" "$(sum "$L/rand3")" "$(dd if="$T/sparse" bs=1 skip=200000 count=3 2>/dev/null | md5 -q)"
ok truncate -s 100 "$T/rand1m"
eq "md5 after truncate to 100" "$(head -c 100 "$L/rand1m" | md5 -q)" "$(sum "$T/rand1m")"
ok truncate -s 70000 "$T/rand1m"
eq "extended part reads as zeros" "$( (head -c 100 "$L/rand1m"; dd if=/dev/zero bs=1 count=69900 2>/dev/null) | md5 -q)" "$(sum "$T/rand1m")"

echo "==> write across block boundaries (64 KiB blocks)"
dd if="$L/rand1m" of="$T/cross" bs=1 skip=65000 count=2000 2>/dev/null
dd if="$L/rand1m" of="$T/cross" bs=1 seek=130000 count=5000 conv=notrunc 2>/dev/null
eq "size cross" 135000 "$(stat -f %z "$T/cross")"
eq "md5 cross" "$( (dd if="$L/rand1m" bs=1 skip=65000 count=2000; dd if=/dev/zero bs=1 count=128000; dd if="$L/rand1m" bs=1 count=5000) 2>/dev/null | md5 -q)" "$(sum "$T/cross")"

echo "==> tree copy and compare"
ok cp -R /usr/share/misc "$T/misc"
ok diff -r /usr/share/misc "$T/misc"
eq "file count" "$(find /usr/share/misc | wc -l | tr -d ' ')" "$(find "$T/misc" | wc -l | tr -d ' ')"

echo "==> read/write/mmap coherence"
cat > "$L/mm.c" <<'CEOF'
#include <sys/mman.h>
#include <sys/stat.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
	char buf[8192], *p; int fd; off_t sz = 65536 + 8192;	/* 9 writes of 8 KiB below */
	if (argc != 2) errx(1, "usage");
	fd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) err(1, "open");
	memset(buf, 'A', sizeof(buf));
	for (off_t o = 0; o < sz; o += sizeof(buf))
		if (pwrite(fd, buf, sizeof(buf), o) != (ssize_t)sizeof(buf)) err(1, "pwrite");
	p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) err(1, "mmap");
	/* 1: write(2) then see it through the mapping */
	memset(buf, 'B', 100);
	if (pwrite(fd, buf, 100, 65536) != 100) err(1, "pwrite2");
	if (p[65536] != 'B' || p[65635] != 'B' || p[65636] != 'A') errx(2, "write not visible through mmap");
	/* 2: store through the mapping, msync, then read(2) */
	memset(p + 1000, 'C', 300);
	if (msync(p, sz, MS_SYNC) != 0) err(1, "msync");
	if (pread(fd, buf, 300, 1000) != 300) err(1, "pread");
	if (buf[0] != 'C' || buf[299] != 'C') errx(3, "mmap store not visible through read");
	/* 3: store without msync, then read(2) (same page cache) */
	memset(p + 70000, 'D', 10);
	if (pread(fd, buf, 10, 70000) != 10) err(1, "pread2");
	if (buf[0] != 'D' || buf[9] != 'D') errx(4, "unsynced mmap store not visible through read");
	if (munmap(p, sz) != 0) err(1, "munmap");
	if (close(fd) != 0) err(1, "close");
	printf("coherence ok\n");
	return 0;
}
CEOF
ok cc -O -o "$L/mm" "$L/mm.c"
ok "$L/mm" "$T/mmfile"
eq "mmfile size (9 pwrites of 8 KiB)" 73728 "$(stat -f %z "$T/mmfile")"
eq "mmfile bytes 1000-1299 after close" "$(printf 'C%.0s' $(seq 1 300) | md5 -q)" "$(dd if="$T/mmfile" bs=1 skip=1000 count=300 2>/dev/null | md5 -q)"
eq "mmfile bytes 70000-70009" "DDDDDDDDDD" "$(dd if="$T/mmfile" bs=1 skip=70000 count=10 2>/dev/null)"

echo "==> remount and re-verify (defeats the guest cache)"
cd / && ok umount "$MNT" && ok mount -t vboxvfs -o uid=0,gid=0 share "$MNT"
eq "md5 rand100k after remount" "$(sum "$L/rand100k")" "$(sum "$T/rand100k")"
eq "md5 cross after remount" "$( (dd if="$L/rand1m" bs=1 skip=65000 count=2000; dd if=/dev/zero bs=1 count=128000; dd if="$L/rand1m" bs=1 count=5000) 2>/dev/null | md5 -q)" "$(sum "$T/cross")"
eq "small after remount" "line 099" "$(sed -n 100p "$T/small")"
ok diff -r /usr/share/misc "$T/misc"

echo "==> fsync and exec from the share"
ok cp /bin/echo "$T/echo"
ok "$T/echo" hello
ok sh -c "cp /bin/sync $T/sync && $T/sync"

echo "==> cleanup"
ok rm -rf "$T"
rm -rf "$L"
echo "==> $fail failure(s)"
[ "$fail" -eq 0 ]
