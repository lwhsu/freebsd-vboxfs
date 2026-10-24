#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Host side: check what the host sees of data a guest writes behind
# (delayed writes), scenario by scenario, with tests/writebehind-guest.pl
# running in the guest.  Times are in ms; guest and host clocks are
# assumed to be within a few hundred ms of each other (VBoxService
# timesync).  Prints "ok"/"FAIL" lines and exits non-zero on any FAIL.
#
# usage: writebehind-check.sh [-s] VM
#   -s  also remount the share with -o sync in the guest and check that
#       writes then go through at once (the share must not be busy)
# env: SHARE (host dir of the share, default ~/vboxfs/share), MNT (guest
#      mount point, default /mnt/share), TREE (guest copy of
#      freebsd-vboxfs, default /root/freebsd-vboxfs)
#
set -u
dosync=0
while getopts s opt; do
	case $opt in
	s) dosync=1 ;;
	*) echo "usage: $0 [-s] VM" >&2; exit 64 ;;
	esac
done
shift $((OPTIND - 1))
VM=${1:?VM}
SHARE=${SHARE:-$HOME/vboxfs/share}; MNT=${MNT:-/mnt/share}; TREE=${TREE:-/root/freebsd-vboxfs}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 $VM"
H=$SHARE/writebehind-$VM; G=$MNT/writebehind-$VM
rm -rf "$H"; mkdir -p "$H" || exit 2
fail=0
ok() { echo "    ok: $*"; }
bad() { echo "    FAIL: $*"; fail=$((fail + 1)); }
now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time * 1000'; }
guest() { $SSH "perl $TREE/tests/writebehind-guest.pl $1 $G"; }
# wait_for FILE STRING TIMEOUT_MS: print the ms when STRING is in FILE, or ""
wait_for() {
	end=$(( $(now_ms) + $3 ))
	while :; do
		if [ -f "$1" ] && grep -q "$2" "$1" 2>/dev/null; then now_ms; return 0; fi
		[ "$(now_ms)" -ge "$end" ] && { echo ""; return 1; }
		sleep 0.05
	done
}
# A guest log: "ok"/"BAD" lines count; BAD lines fail.
guest_result() {
	grep -c '^ok' "$1" | sed 's/^/        guest ok lines: /'
	if grep -q '^BAD' "$1"; then grep '^BAD' "$1" | sed 's/^/        /'; bad "$2: guest reported BAD"; else ok "$2: guest"; fi
}
scen_delayed() {
	# $1: label, $2: max lag for the first write (ms)
	guest delayed > "$H/.delayed.out" 2>&1 & gpid=$!
	t_hello=$(wait_for "$H/delayed.txt" hello 8000)
	t_world=$(wait_for "$H/delayed.txt" world 8000)
	wait $gpid
	w_hello=$(awk '/^wrote hello/ { print $3 }' "$H/.delayed.out")
	w_world=$(awk '/^wrote world/ { print $3 }' "$H/.delayed.out")
	if [ -n "$t_hello" ] && [ -n "$w_hello" ]; then
		lag=$((t_hello - w_hello)); echo "        first write visible after $lag ms"
		[ "$lag" -le "$2" ] && ok "$1: first write visible within $2 ms" || bad "$1: first write took $lag ms (limit $2)"
	else
		bad "$1: first write never visible"
	fi
	if [ -n "$t_world" ] && [ -n "$w_world" ]; then
		lag=$((t_world - w_world)); echo "        write before close visible after $lag ms"
		[ "$lag" -le 1000 ] && ok "$1: close(2) made the rest visible" || bad "$1: write before close took $lag ms"
	else
		bad "$1: write before close never visible"
	fi
	guest_result "$H/.delayed.out" "$1"
	rm -f "$H/delayed.txt"
}

echo "==> delayed write, host visibility (writedelay $($SSH sysctl -n vfs.vboxvfs.writedelay) s)"
scen_delayed "delayed" 2500

echo "==> 64 x 4 KiB sequential writes"
guest seq4k > "$H/.seq4k.out" 2>&1
hm=$(md5 -q "$H/seq4k.bin" 2>/dev/null); gm=$($SSH "md5 -q $G/seq4k.bin")
[ -n "$hm" ] && [ "$hm" = "$gm" ] && ok "seq4k: host and guest md5 agree" || bad "seq4k: md5 host=$hm guest=$gm"
guest_result "$H/.seq4k.out" "seq4k"

echo "==> write, unlink, read back, close"
guest unlink > "$H/.unlink.out" 2>&1
[ ! -e "$H/tmpf" ] && ok "unlink: nothing left on the host" || bad "unlink: tmpf left on the host"
guest_result "$H/.unlink.out" "unlink"

echo "==> rename over an open file with data behind"
guest renameover > "$H/.renameover.out" 2>&1
[ "$(cat "$H/target" 2>/dev/null)" = "new content" ] && ok "renameover: host has the new content" || bad "renameover: host content: $(cat "$H/target" 2>/dev/null | head -1)"
guest_result "$H/.renameover.out" "renameover"

echo "==> truncate with data behind"
guest truncate > "$H/.truncate.out" 2>&1
sz=$(stat -f %z "$H/trunc.bin" 2>/dev/null)
[ "$sz" = 5000 ] && ok "truncate: host size 5000" || bad "truncate: host size $sz"
[ "$(head -c 10 "$H/trunc.bin" 2>/dev/null)" = "0123456789" ] && ok "truncate: host head rewritten" || bad "truncate: host head: $(head -c 10 "$H/trunc.bin")"
guest_result "$H/.truncate.out" "truncate"

echo "==> fsync pushes data behind"
guest fsync > "$H/.fsync.out" 2>&1 & gpid=$!
t=$(wait_for "$H/fsync.txt" fsynced 6000); wait $gpid
s=$(awk '/^synced/ { print $2 }' "$H/.fsync.out")
if [ -n "$t" ] && [ -n "$s" ]; then lag=$((t - s)); echo "        visible $lag ms after fsync returned"; [ "$lag" -le 500 ] && ok "fsync: visible at once" || bad "fsync: $lag ms"; else bad "fsync: never visible"; fi
guest_result "$H/.fsync.out" "fsync"

echo "==> sync(2) pushes data behind"
guest syncall > "$H/.syncall.out" 2>&1 & gpid=$!
t=$(wait_for "$H/syncall.txt" syncall 6000); wait $gpid
s=$(awk '/^synced/ { print $2 }' "$H/.syncall.out")
if [ -n "$t" ] && [ -n "$s" ]; then lag=$((t - s)); echo "        visible $lag ms after sync returned"; [ "$lag" -le 500 ] && ok "syncall: visible at once" || bad "syncall: $lag ms"; else bad "syncall: never visible"; fi
guest_result "$H/.syncall.out" "syncall"

echo "==> O_SYNC writes go through"
guest osync > "$H/.osync.out" 2>&1 & gpid=$!
t=$(wait_for "$H/osync.txt" osync 6000); wait $gpid
s=$(awk '/^written/ { print $2 }' "$H/.osync.out")
if [ -n "$t" ] && [ -n "$s" ]; then lag=$((t - s)); echo "        visible $lag ms after write returned"; [ "$lag" -le 500 ] && ok "osync: visible at once" || bad "osync: $lag ms"; else bad "osync: never visible"; fi
guest_result "$H/.osync.out" "osync"

echo "==> a growing log file, never closed while it grows"
guest append > "$H/.append.out" 2>&1 & gpid=$!
maxlag=0; seen=0; end=$(( $(now_ms) + 4500 ))
while [ "$(now_ms)" -lt "$end" ]; do
	last=$(tail -1 "$H/append.log" 2>/dev/null)
	case "$last" in
	line*) n=$(echo "$last" | awk '{ print $2 }'); ts=$(echo "$last" | awk '{ print $3 }')
		if [ "$n" -gt "$seen" ]; then seen=$n; fi
		lag=$(( $(now_ms) - ts )); [ "$lag" -gt "$maxlag" ] && [ "$n" -lt 40 ] && maxlag=$lag ;;
	esac
	sleep 0.1
done
wait $gpid
echo "        host saw up to line $seen while the file was growing; max lag behind the last visible line $maxlag ms"
[ "$seen" -ge 30 ] && ok "append: host followed the growth" || bad "append: host saw only $seen of 40 lines while growing"
[ "$(wc -l < "$H/append.log")" -eq 40 ] && ok "append: all 40 lines after close" || bad "append: $(wc -l < "$H/append.log") lines after close"
guest_result "$H/.append.out" "append"

echo "==> stat(2) while data is behind"
guest stat > "$H/.stat.out" 2>&1
guest_result "$H/.stat.out" "stat"

if [ $dosync -eq 1 ]; then
	echo "==> mount -o sync: writes go through at once"
	if $SSH "umount $MNT && $TREE/mount_vboxvfs/mount_vboxvfs -o sync share $MNT && mount -p | grep vboxvfs"; then
		scen_delayed "sync mount" 500
	else
		bad "sync mount: remount with -o sync failed"
	fi
	$SSH "mount -t vboxvfs | grep -q $MNT && umount $MNT; $TREE/mount_vboxvfs/mount_vboxvfs share $MNT && mount -p | grep vboxvfs"
fi

echo "==> guest dmesg tail"; $SSH "dmesg | tail -3 | grep -iv 'host features'" || true
rm -rf "$H"
echo "==> $fail failure(s)"
[ $fail -eq 0 ]
