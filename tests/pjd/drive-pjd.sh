#!/bin/sh
# Host-side driver for run-pjd.sh: mounts the share in the guest, starts the
# runner, watches the console log for panics and the progress file for
# wedges, and resets the VM / relaunches the runner as needed.  Logs to
# stdout; append-only notes go to $OUT/resets.txt.
#
# usage: drive-pjd.sh VM OUT [T]
#   VM   guest name (ssh host alias == VirtualBox VM name)
#   OUT  host directory for this run's results (created)
#   T    per-test timeout in seconds (default 180)
# env: RES (guest results dir, default /root/results-pjd-<VM>), TESTDIR
#      (guest test dir, default /mnt/share/pjd-<VM>), KO, MNT_VBOXVFS,
#      TREE (guest copy of freebsd-vboxfs, default /root/freebsd-vboxfs),
#      MAXSEC (give up after this many seconds, default 10800)
VM=${1:?VM name}
OUT=${2:?host results dir}
T=${3:-180}
TREE=${TREE:-/root/freebsd-vboxfs}
KO=${KO:-$TREE/vboxvfs/vboxvfs.ko}
MNT_VBOXVFS=${MNT_VBOXVFS:-$TREE/mount_vboxvfs/mount_vboxvfs}
RES=${RES:-/root/results-pjd-$VM}
TESTDIR=${TESTDIR:-/mnt/share/pjd-$VM}
MAXSEC=${MAXSEC:-10800}
VMCTL=$(dirname "$0")/../../scripts/vm/vmctl.sh
CON=$HOME/vboxfs/vm/$VM-console.log
SSH="ssh -o BatchMode=yes -o ConnectTimeout=10 $VM"
mkdir -p "$OUT"
resets=0
start=$(date +%s)
log() { echo "$(date -u +%H:%M:%S) +$(( ($(date +%s) - start) / 60 ))m $*"; }

wait_ssh() {
	st=$(VBoxManage showvminfo $VM --machinereadable 2>/dev/null | sed -n "s/^VMState=\"\(.*\)\"/\1/p")
	case "$st" in poweroff|aborted|saved) log "VM state $st: starting it"; $VMCTL start $VM >/dev/null 2>&1; sleep 10 ;; esac
	n=0
	until $SSH true 2>/dev/null; do n=$((n + 1)); [ $n -ge 60 ] && return 1; sleep 5; done
	return 0
}
remount_and_launch() {
	wait_ssh || { log "ssh not reachable"; exit 1; }
	# $KO/$MNT_VBOXVFS/$TESTDIR/$RES/$T/$TREE expand here; \$n and \$(...) on the guest
	out=$($SSH "mount -t vboxvfs | grep -q ' on /mnt/share ' || { kldstat -q -n vboxvfs || kldload $KO; $MNT_VBOXVFS -o uid=0,gid=0 share /mnt/share; }; n=\$(mount -t vboxvfs | grep -c ' on /mnt/share '); [ \$n -eq 1 ] || { echo \"MOUNTED \$n TIMES, refusing to run\"; exit 1; }; mkdir -p $TESTDIR $RES; env RES=$RES TESTDIR=$TESTDIR T=$T nohup sh $TREE/tests/pjd/run-pjd.sh > $RES/run.out 2>&1 < /dev/null &" 2>&1)
	rc=$?
	[ -n "$out" ] && log "launch: $out"
	[ $rc -eq 0 ] || { log "launch FAILED (rc $rc), not retrying"; exit 1; }
}
reset_vm() {
	resets=$((resets + 1))
	log "RESET #$resets ($1)"
	$SSH 'sync; sync' 2>/dev/null
	echo "$(date -u) reset #$resets reason: $1" >> "$OUT/resets.txt"
	VBoxManage controlvm $VM poweroff; sleep 5; $VMCTL start $VM >/dev/null
	sleep 15
	wait_ssh || { log "ssh did not come back after reset"; exit 1; }
	remount_and_launch
}

count_panics() { /usr/bin/grep -cE '^panic:|^Fatal trap' "$CON" 2>/dev/null || true; }
panics=$(count_panics); panics=${panics:-0}
remount_and_launch
lastline=""; stuck=0; dead=0
while :; do
	sleep 10
	now=$(count_panics); now=${now:-0}
	if [ "$now" != "$panics" ]; then
		panics=$now
		log "PANIC seen in console log (total $panics):"; /usr/bin/grep -nE '^panic:|^Fatal trap' "$CON" | tail -1
		echo "$(date -u) panic: $(/usr/bin/grep -E '^panic:|^Fatal trap' "$CON" | tail -1)" >> "$OUT/resets.txt"
		# the guest dumps and reboots by itself; wait for ssh then relaunch
		sleep 30; wait_ssh || { log "ssh did not come back after panic"; exit 1; }
		remount_and_launch
		continue
	fi
	out=$($SSH "tail -1 $RES/progress.txt 2>/dev/null; [ -f $RES/end.txt ] && echo DONE; echo ALIVE" 2>/dev/null)
	case "$out" in
	*ALIVE*) dead=0 ;;
	*)
		dead=$((dead + 1))
		[ $dead -ge 9 ] && { reset_vm "ssh unreachable for 90s"; dead=0; }
		continue ;;
	esac
	out=$(echo "$out" | grep -v '^ALIVE$')
	line=$(echo "$out" | head -1)
	echo "$out" | /usr/bin/grep -q DONE && { log "RUN FINISHED after $resets resets"; break; }
	case "$line" in
	*WEDGED*)
		reset_vm "wedged: $line"; lastline=""; stuck=0; continue ;;
	esac
	if [ "$line" = "$lastline" ]; then
		stuck=$((stuck + 1))
		# runner should have moved on after T+20s; allow T+60s
		if [ $stuck -ge $(( (T + 60) / 10 )) ]; then reset_vm "no progress: $line"; stuck=0; lastline=""; continue; fi
	else
		stuck=0; lastline="$line"
		log "$line"
	fi
	[ $(( $(date +%s) - start )) -ge $MAXSEC ] && { log "GIVING UP after ${MAXSEC}s"; break; }
done
log "done; resets=$resets"
