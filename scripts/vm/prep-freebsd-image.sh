#!/bin/sh
#
# SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Prepare an official FreeBSD VM image (raw) for use as a headless test
# guest: install an ssh key for root, set the hostname, enable sshd and
# crash dumps, send the console to the first serial port, and make a
# panic dump and reboot instead of waiting in ddb.
#
# usage: prep-freebsd-image.sh image.raw hostname mdunit pubkey-file
#
# Needs root (mdconfig/mount); run via sudo or as root.
#
set -eu

if [ $# -ne 4 ]; then
	echo "usage: $0 image.raw hostname mdunit pubkey-file" >&2
	exit 64
fi
RAW=$1
HOST=$2
UNIT=$3
PUBKEY=$4
MNT=/mnt/vboxfs-prep-$HOST

if [ "$(id -u)" -ne 0 ]; then
	exec sudo "$0" "$@"
fi

cleanup() {
	umount "$MNT" 2>/dev/null || true
	mdconfig -d -u "$UNIT" 2>/dev/null || true
}
trap cleanup EXIT

mdconfig -a -t vnode -f "$RAW" -u "$UNIT"
gpart show -p "md$UNIT"
PART=$(gpart show -p "md$UNIT" | awk '$4 == "freebsd-ufs" { print $3 }')
if [ -z "$PART" ]; then
	echo "$0: no freebsd-ufs partition found on md$UNIT" >&2
	exit 1
fi
mkdir -p "$MNT"
mount "/dev/$PART" "$MNT"

install -d -m 700 "$MNT/root/.ssh"
install -m 600 "$PUBKEY" "$MNT/root/.ssh/authorized_keys"

cat >> "$MNT/etc/rc.conf" <<EOT

# added by prep-freebsd-image.sh
hostname="$HOST"
sshd_enable="YES"
dumpdev="AUTO"
EOT

sed -i '' 's/^#*PermitRootLogin.*/PermitRootLogin prohibit-password/' \
    "$MNT/etc/ssh/sshd_config"

cat >> "$MNT/boot/loader.conf" <<'EOT'

# added by prep-freebsd-image.sh
console="comconsole,efi"
boot_multicons="YES"
autoboot_delay="3"
EOT

cat >> "$MNT/etc/sysctl.conf" <<'EOT'

# added by prep-freebsd-image.sh: dump and reboot on panic instead of
# waiting in ddb (the serial log still gets the KDB_TRACE backtrace)
debug.debugger_on_panic=0
kern.panic_reboot_wait_time=15
EOT

echo "== $MNT/etc/rc.conf =="
cat "$MNT/etc/rc.conf"
umount "$MNT"
mdconfig -d -u "$UNIT"
trap - EXIT
echo "$RAW prepared for $HOST"
