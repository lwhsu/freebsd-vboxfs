#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Host side: force-unmount the share in a guest while files are open in it
# with data written behind, at the root and two directories deep (a forced
# vflush() reclaims vnodes in list order, parents before children at times),
# then check that the guest survived, that the host got the data, and that
# the share mounts and works again.  Prints "ok"/"FAIL" lines and exits
# non-zero on any FAIL.
#
# usage: umount-force-check.sh VM
# env: SHARE (host dir of the share, default ~/vboxfs/share), MNT (guest
#      mount point, default /mnt/share), MOUNT_CMD (guest command that
#      mounts the share again, default "/root/freebsd-vboxfs/mount_vboxvfs/
#      mount_vboxvfs share $MNT")
#
set -u
VM=${1:?VM}
SHARE=${SHARE:-$HOME/vboxfs/share}; MNT=${MNT:-/mnt/share}
MOUNT_CMD=${MOUNT_CMD:-/root/freebsd-vboxfs/mount_vboxvfs/mount_vboxvfs share $MNT}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 $VM"
H=$SHARE/umountf-$VM; G=$MNT/umountf-$VM
rm -rf "$H"; mkdir -p "$H/d1/d2" || exit 2
fail=0
ok() { echo "    ok: $*"; }
bad() { echo "    FAIL: $*"; fail=$((fail + 1)); }
boot0=$($SSH "sysctl -n kern.boottime" | sed 's/.*sec = \([0-9]*\).*/\1/')
echo "==> guest boottime $boot0; open root.txt and d1/d2/deep.txt, write, keep them open"
$SSH "perl -e '
	use Fcntl;
	sysopen(my \$a, q{$G/root.txt}, O_WRONLY|O_CREAT|O_TRUNC, 0644) or die;
	sysopen(my \$b, q{$G/d1/d2/deep.txt}, O_WRONLY|O_CREAT|O_TRUNC, 0644) or die;
	syswrite(\$a, qq{root behind\n}); syswrite(\$b, qq{deep behind\n});
	print qq{written\n}; \$| = 1;
	sleep 20;
	print qq{closes: }, (close(\$a) ? q{ok} : qq{\$!}), q{ }, (close(\$b) ? q{ok} : qq{\$!}), qq{\n};
'" > "$H/.guest.out" 2>&1 & gpid=$!
i=0; while ! grep -q '^written' "$H/.guest.out" 2>/dev/null; do sleep 0.1; i=$((i + 1)); [ $i -gt 100 ] && break; done
grep -q '^written' "$H/.guest.out" && ok "files open with data behind" || bad "guest did not report the writes: $(cat "$H/.guest.out")"
echo "==> umount -f with the files open"
out=$($SSH "umount -f $MNT 2>&1; echo rc=\$?; mount -t vboxvfs | grep -c ' on $MNT ' ; kldstat -q -n vboxvfs && echo module-loaded" 2>&1)
echo "$out" | sed 's/^/        /'
echo "$out" | grep -q '^rc=0' && ok "umount -f returned 0" || bad "umount -f: $(echo "$out" | head -1)"
sleep 1
boot1=$($SSH "sysctl -n kern.boottime" 2>/dev/null | sed 's/.*sec = \([0-9]*\).*/\1/')
[ -n "$boot1" ] && [ "$boot1" = "$boot0" ] && ok "guest still up (no reboot)" || bad "guest unreachable or rebooted (boottime $boot0 -> ${boot1:-?})"
[ "$(cat "$H/root.txt" 2>/dev/null)" = "root behind" ] && ok "host has the root file's data" || bad "host root.txt: [$(cat "$H/root.txt" 2>/dev/null)]"
[ "$(cat "$H/d1/d2/deep.txt" 2>/dev/null)" = "deep behind" ] && ok "host has the deep file's data" || bad "host deep.txt: [$(cat "$H/d1/d2/deep.txt" 2>/dev/null)]"
echo "==> mount again and use the share"
out=$($SSH "$MOUNT_CMD && mount -p | grep -c vboxvfs && ls $G/d1/d2 && echo again > $G/again.txt && cat $G/again.txt" 2>&1)
echo "$out" | sed 's/^/        /'
echo "$out" | grep -q '^again$' && ok "share mounted and usable again" || bad "remount or use failed"
kill $gpid 2>/dev/null; wait $gpid 2>/dev/null
echo "==> guest dmesg tail"; $SSH "dmesg | tail -4 | grep -iv 'host features'" | sed 's/^/        /'
$SSH "dmesg | grep -ci 'panic\|lock order reversal\|witness'" | sed 's/^/        panic\/LOR\/witness lines in dmesg (whole uptime): /'
rm -rf "$H"
echo "==> $fail failure(s)"
[ $fail -eq 0 ]
