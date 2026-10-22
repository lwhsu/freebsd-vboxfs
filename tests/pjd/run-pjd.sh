#!/bin/sh
# Guest-side pjdfstest runner: every .t file one at a time under a timeout,
# resumable after a panic or a VM reset (already finished tests are skipped,
# a test that was started but never finished is recorded as INTERRUPTED).
# Each .t runs in its own fresh directory $TESTDIR/b-<test>/t so that a
# wedge stays with one test and the layout matches the baselines.  A test
# process that cannot be killed (stuck in D state) is recorded as WEDGED and
# the runner exits so the host side (drive-pjd.sh) can reset the VM.
#
# env: RES (results dir, default /root/results-pjd), TESTDIR (default
#      /mnt/share/pjd-<hostname>), PJD (pjdfstest checkout, default
#      /root/pjdfstest), T (per-test timeout in seconds, default 180)
RES=${RES:-/root/results-pjd}
TESTDIR=${TESTDIR:-/mnt/share/pjd-$(hostname -s)}
PJD=${PJD:-/root/pjdfstest}
T=${T:-180}
mkdir -p $RES/log
cd /root || exit 1
[ -f $RES/start.txt ] || date -u > $RES/start.txt

wedged() {	# $1 = label
	{
		echo "=== $1 WEDGED $(date -u) ==="
		ps -axo pid,stat,wchan,etime,command | awk 'NR == 1 || $2 ~ /^D/'
		for p in $(ps -axo pid,stat | awk '$2 ~ /^D$/ { print $1 }'); do procstat -kk $p; done
	} >> $RES/wedged.txt 2>&1
	echo "$1 WEDGED $(date -u +%s)" >> $RES/progress.txt
	sync; sync
	exit 2
}
# run "$@" in the background, wait up to $1 seconds, return 0 if it exited
# (exit status in $rc), 1 if it is still alive (unkillable)
run_bounded() {
	limit=$1; shift
	"$@" &
	pid=$!
	i=0
	while kill -0 $pid 2>/dev/null && [ $i -lt $limit ]; do sleep 1; i=$((i + 1)); done
	if kill -0 $pid 2>/dev/null; then kill -9 $pid 2>/dev/null; sleep 2; kill -0 $pid 2>/dev/null && return 1; fi
	wait $pid; rc=$?
	return 0
}

prev=none
for t in $(cd $PJD/tests && find . -name '*.t' | sort); do
	grep -q "^$t " $RES/rc.txt 2>/dev/null && { prev=$t; continue; }
	if grep -q "^$t START" $RES/progress.txt 2>/dev/null; then
		# was started but never finished: the guest went down during this test
		echo "$t INTERRUPTED" >> $RES/rc.txt
		echo "$t INTERRUPTED $(date -u +%s)" >> $RES/progress.txt
		prev=$t; continue
	fi
	mkdir -p $RES/log/$(dirname $t)
	id=b-$(echo "$t" | sed -e 's|^\./||' -e 's|/|-|g' -e 's|\.t$||')
	# setup on the shared directory: if this wedges, blame the previous test
	run_bounded 60 mkdir -p $TESTDIR/$id/t || wedged "$t SETUP-after-$prev"
	[ $rc -eq 0 ] || { echo "$t MKDIR-FAILED" >> $RES/rc.txt; prev=$t; continue; }
	echo "$t START $(date -u +%s)" >> $RES/progress.txt
	run_bounded $((T + 20)) sh -c "cd $TESTDIR/$id/t && exec timeout -s KILL $T prove -v $PJD/tests/$t" > $RES/log/$t.log 2>&1 || { echo "$t WEDGED" >> $RES/rc.txt; wedged "$t"; }
	echo "$t $rc" >> $RES/rc.txt
	echo "$t END $rc $(date -u +%s)" >> $RES/progress.txt
	prev=$t
done
date -u > $RES/end.txt
sync
