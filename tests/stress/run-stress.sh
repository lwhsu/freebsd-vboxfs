#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Host side of the stress run: records host-side baselines, runs
# host-churn.sh against the share while the guest runs stress-10min.sh, then
# collects the guest logs, the VBox.log shared-folder lines and the console
# log into OUT.
#
# usage: run-stress.sh VM SECONDS OUT
# env: SHARE (host dir of the share, default ~/vboxfs/share), MNT (guest
#      mount point, default /mnt/share), TREE (guest copy of freebsd-vboxfs,
#      default /root/freebsd-vboxfs)
VM=${1:?VM}; SECS=${2:?seconds}; OUT=${3:?out dir}
SHARE=${SHARE:-$HOME/vboxfs/share}; MNT=${MNT:-/mnt/share}; TREE=${TREE:-/root/freebsd-vboxfs}
HERE=$(cd "$(dirname "$0")" && pwd)
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 $VM"
mkdir -p "$OUT" || exit 1
pid=$(pgrep -f "VBoxHeadless.*--comment $VM " | head -1)
{
	echo "== $(date -u) host baseline =="; uname -a; VBoxManage --version
	[ -n "$pid" ] && { echo "VBoxHeadless pid $pid, open files: $(procstat -f $pid 2>/dev/null | wc -l | tr -d ' ')"; ps -o rss,vsz,%cpu -p $pid; }
	vmstat -m | grep -iE 'vbox' ; df -k "$SHARE" | tail -1
} > "$OUT/host-baseline.txt" 2>&1
con=$HOME/vboxfs/vm/$VM-console.log; conlines=$( [ -f "$con" ] && wc -l < "$con" || echo 0 )
vlog=$HOME/vboxfs/vm/$VM/Logs/VBox.log; vlines=$( [ -f "$vlog" ] && wc -l < "$vlog" || echo 0 )
mkdir -p "$SHARE/churn-$VM"
sh "$HERE/host-churn.sh" "$SHARE/churn-$VM" $((SECS + 10)) > "$OUT/host-churn.log" 2>&1 & churn=$!
echo "$(date -u +%T) guest run starting" | tee "$OUT/run.log"
$SSH "rm -rf /root/stress-logs/current; env TAG=$VM sh $TREE/tests/stress/stress-10min.sh $MNT $SECS /root/stress-logs/current; echo GUEST-RC=\$?" 2>&1 | LC_ALL=C tr -cd '\11\12\15\40-\176' | tee -a "$OUT/run.log"
rc=$(sed -n 's/^GUEST-RC=//p' "$OUT/run.log" | tail -1); rc=${rc:-1}
echo "$(date -u +%T) guest run finished, rc=$rc" | tee -a "$OUT/run.log"
wait $churn; cat "$OUT/host-churn.log"
scp -q -r "$VM:/root/stress-logs/current/" "$OUT/guest-logs" 2>&1
rm -rf "$SHARE/churn-$VM"
{
	echo "== $(date -u) host after =="
	[ -n "$pid" ] && { echo "VBoxHeadless open files: $(procstat -f $pid 2>/dev/null | wc -l | tr -d ' ')"; ps -o rss,vsz,%cpu -p $pid; }
	echo "== VBox.log new lines mentioning shared folders / errors =="
	[ -f "$vlog" ] && tail -n +$((vlines + 1)) "$vlog" | grep -iE 'shared|shfl|sharedfolder|error|warn' | head -40
	echo "== console log new lines =="
	[ -f "$con" ] && tail -n +$((conlines + 1)) "$con" | LC_ALL=C tr -cd '\11\12\15\40-\176' | grep -vE '^$' | tail -60
} > "$OUT/host-after.txt" 2>&1
exit $rc
