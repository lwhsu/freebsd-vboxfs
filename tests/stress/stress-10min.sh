#!/bin/sh
# Guest side of the stress run (notes/04 "壓力測試"): for SECONDS run, all at
# once on the vboxvfs mount MNT,
#   - two fsx loops (one with mmap, one without), different seeds each pass,
#   - nine stress2 test cases (creat mkdir rename dirrename rw mmap fts
#     symlink openat; lockf is left out: it busy-waits),
#   - a "real workload" loop: copy a tree in, diff -r it against the
#     original, git clone a repository from the host, git status/gc, rm -rf,
#   - guest-watch.sh on MNT/churn while the host runs host-churn.sh there.
# Before and after: malloc statistics, mount/module state, dmesg; the mount
# must be a single vboxvfs layer.  Everything is logged under LOGDIR (local
# disk); a summary is printed at the end and the exit status is 0 only if
# every component finished cleanly and nothing suspicious showed up in dmesg.
#
# usage: stress-10min.sh MNT SECONDS [LOGDIR]
# env: FSX (default /root/fsx), STRESS2 (default /root/stress2),
#      GITURL (default git://10.0.2.2/pjdfstest), TTLMS (default 200),
#      TAG (suffix of the directories used on the share, default the host
#      name, so that two guests of one host can run at the same time),
#      NO_UNMOUNT=1 to leave the share mounted at the end
MNT=${1:?mount point}; SECS=${2:?seconds}; LOGDIR=${3:-/root/stress-logs/$(date -u +%Y%m%dT%H%M%SZ)}
FSX=${FSX:-/root/fsx}; STRESS2=${STRESS2:-/root/stress2}; GITURL=${GITURL:-git://10.0.2.2/pjdfstest}
TTLMS=${TTLMS:-200}; TAG=${TAG:-$(hostname -s)}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$LOGDIR" || exit 1
log() { echo "$(date -u +%T) $*" | tee -a "$LOGDIR/summary.txt"; }
fail=0
problem() { log "PROBLEM: $*"; fail=$((fail + 1)); }

# --- preconditions ---------------------------------------------------------
n=$(mount -t vboxvfs | grep -c " on $MNT ")
[ "$n" -eq 1 ] || { echo "$MNT: $n vboxvfs mount layers (need exactly 1)" >&2; exit 2; }
[ -x "$FSX" ] || { echo "no fsx at $FSX" >&2; exit 2; }
for t in creat mkdir rename dirrename rw mmap fts symlink openat; do
	[ -x "$STRESS2/testcases/$t/$t" ] || { echo "no stress2 binary $STRESS2/testcases/$t/$t" >&2; exit 2; }
done
W=$MNT/stress-work-$TAG; X=$MNT/stressX-$TAG; CH=$MNT/churn-$TAG
rm -rf "$W" "$X" 2>/dev/null; mkdir -p "$W" "$X" "$CH" || exit 2
ln -s /nonexistent "$W/probe-symlink" 2>/dev/null && rm "$W/probe-symlink" && symlinks=yes || symlinks=no

# --- baseline --------------------------------------------------------------
{
	echo "== $(date -u) baseline =="; uname -a; mount -t vboxvfs; kldstat | grep -E 'vboxguest|vboxvfs'
	sysctl vfs.vboxvfs 2>/dev/null; vmstat -m | grep -iE 'vboxvfs|iprtheap'; sysctl vfs.numvnodes vfs.freevnodes
	df -k "$MNT" | tail -1; echo "symlink creation: $symlinks"; dmesg | wc -l
} > "$LOGDIR/baseline.txt" 2>&1
dmesg_before=$(dmesg | wc -l | tr -d ' ')
start=$(date +%s); end=$((start + SECS))
log "start: $SECS s on $MNT, logs in $LOGDIR"

# --- fsx loops -------------------------------------------------------------
fsx_loop() {	# $1 = name, $2 = extra flags, $3 = seed base
	i=0
	while [ "$(date +%s)" -lt $end ]; do
		i=$((i + 1))
		if ! "$FSX" -N 20000 -S $(( $3 + i )) $2 -q "$W/$1" >> "$LOGDIR/$1.log" 2>&1; then
			echo "pass $i (seed $(( $3 + i ))) FAILED rc=$?" >> "$LOGDIR/$1.log"; echo "FAILED" > "$LOGDIR/$1.status"; return 1
		fi
	done
	echo "ok $i passes" > "$LOGDIR/$1.status"
}
fsx_loop fsx-mmap "" 1000 & p_fsx1=$!
fsx_loop fsx-nomap "-R -W" 2000 & p_fsx2=$!

# --- stress2 ---------------------------------------------------------------
rm -rf /tmp/s2.* 2>/dev/null
S2C="-t ${SECS}s -l 100 -n -h -v -v -d $X"
s2() {	# $1 = test, $2 = incarnations, $3 = KBLOCKS, $4 = INODES
	( cd "$STRESS2/testcases" && env CTRLDIR=/tmp/s2.$1 KBLOCKS=$3 INODES=$4 "$1/$1" $S2C -i $2 > "$LOGDIR/s2-$1.log" 2>&1; echo "rc=$?" >> "$LOGDIR/s2-$1.log" ) &
	eval "p_s2_$1=$!"
}
s2 creat 2 65536 100000
s2 mkdir 2 65536 22000
s2 rename 2 65536 100000
s2 dirrename 2 65536 100000
s2 rw 2 131072 1000
s2 mmap 2 65536 1000
s2 fts 1 65536 1000
s2 openat 2 65536 1000
[ $symlinks = yes ] && s2 symlink 2 65536 4000

# --- real workload loop ----------------------------------------------------
workload() {
	i=0
	while [ "$(date +%s)" -lt $end ]; do
		i=$((i + 1)); d=$W/wl.$i
		mkdir -p "$d" || { echo "pass $i: mkdir failed" >> "$LOGDIR/workload.log"; echo FAILED > "$LOGDIR/workload.status"; return 1; }
		( cd /usr/share && tar cf - misc mk 2>/dev/null ) | ( cd "$d" && tar xf - ) || echo "pass $i: tar copy failed" >> "$LOGDIR/workload.log"
		diff -r /usr/share/misc "$d/misc" >> "$LOGDIR/workload.log" 2>&1 || { echo "pass $i: diff -r misc FAILED" >> "$LOGDIR/workload.log"; echo FAILED > "$LOGDIR/workload.status"; return 1; }
		diff -r /usr/share/mk "$d/mk" >> "$LOGDIR/workload.log" 2>&1 || { echo "pass $i: diff -r mk FAILED" >> "$LOGDIR/workload.log"; echo FAILED > "$LOGDIR/workload.status"; return 1; }
		if git clone -q "$GITURL" "$d/repo" >> "$LOGDIR/workload.log" 2>&1; then
			( cd "$d/repo" && git status --porcelain | head -3 && git gc -q ) >> "$LOGDIR/workload.log" 2>&1 || echo "pass $i: git status/gc failed" >> "$LOGDIR/workload.log"
			st=$(cd "$d/repo" && git status --porcelain | wc -l | tr -d ' ')
			[ "$st" -eq 0 ] || { echo "pass $i: git status not clean ($st entries)" >> "$LOGDIR/workload.log"; echo FAILED > "$LOGDIR/workload.status"; return 1; }
		else
			echo "pass $i: git clone failed" >> "$LOGDIR/workload.log"
		fi
		rm -rf "$d" || { echo "pass $i: rm -rf failed" >> "$LOGDIR/workload.log"; echo FAILED > "$LOGDIR/workload.status"; return 1; }
		echo "pass $i ok $(date -u +%T)" >> "$LOGDIR/workload.log"
	done
	echo "ok $i passes" > "$LOGDIR/workload.status"
}
workload & p_wl=$!

# --- host/guest concurrency watcher ---------------------------------------
sh "$HERE/guest-watch.sh" "$CH" $((SECS - 5)) $TTLMS > "$LOGDIR/guest-watch.log" 2>&1 & p_watch=$!

# --- wait ------------------------------------------------------------------
wait $p_fsx1; r=$?; log "fsx-mmap: $(cat "$LOGDIR/fsx-mmap.status" 2>/dev/null) (rc $r)"; [ $r -eq 0 ] || problem "fsx-mmap failed"
wait $p_fsx2; r=$?; log "fsx-nomap: $(cat "$LOGDIR/fsx-nomap.status" 2>/dev/null) (rc $r)"; [ $r -eq 0 ] || problem "fsx-nomap failed"
wait $p_wl; r=$?; log "workload: $(cat "$LOGDIR/workload.status" 2>/dev/null) (rc $r)"; [ $r -eq 0 ] || problem "workload failed"
wait $p_watch; r=$?; log "guest-watch: $(tail -1 "$LOGDIR/guest-watch.log") (rc $r)"; [ $r -eq 0 ] || problem "guest-watch failed"
for t in creat mkdir rename dirrename rw mmap fts openat symlink; do
	eval "p=\${p_s2_$t:-}"; [ -n "$p" ] || continue
	# stress2 stops itself at -t; allow 5 minutes of overrun, then kill
	n=0; while kill -0 $p 2>/dev/null && [ $n -lt 300 ]; do sleep 1; n=$((n + 1)); done
	if kill -0 $p 2>/dev/null; then problem "stress2 $t still running 5 min after its time limit (hang?)"; procstat -kk $p >> "$LOGDIR/s2-$t.log" 2>&1; kill -9 $p 2>/dev/null; fi
	wait $p 2>/dev/null
	rc=$(sed -n 's/^rc=//p' "$LOGDIR/s2-$t.log" | tail -1)
	# "<name>(size=N, incarnations=M). Free(...)": size is absent for tests
	# without a size (mmap, fts, openat); the status line is " <name>: run
	# time ..." with the name cut to 8 characters.
	setup=$(grep -E "^[a-z]+\((size=|incarnations=)" "$LOGDIR/s2-$t.log" | head -1)
	size=$(echo "$setup" | grep -oE "size=[0-9]+")
	if [ "$t" = fts ]; then
		# fts walks while the other tests create and delete: ENOENT on a
		# vanished entry is the expected race, anything else is not
		races=$(grep -cE "^fts: .*: No such file or directory$" "$LOGDIR/s2-$t.log")
		odd=$(grep -vE "^ *[a-z]{1,8}: run time|^[a-z]+\((size=|incarnations=)|^rc=|^fts: .*: No such file or directory$" "$LOGDIR/s2-$t.log" | grep -c .)
		log "stress2 $t: rc=${rc:-?} ${size:-nosize} vanished-entry-races=$races other-lines=$odd"
	else
		odd=$(grep -vE "^ *[a-z]{1,8}: run time|^[a-z]+\((size=|incarnations=)|^rc=" "$LOGDIR/s2-$t.log" | grep -c .)
		log "stress2 $t: rc=${rc:-?} ${size:-nosize} other-lines=$odd"
	fi
	[ "${rc:-1}" -eq 0 ] || problem "stress2 $t rc=$rc"
	[ -n "$setup" ] || [ "$t" = fts ] || problem "stress2 $t never printed its setup line (did nothing?)"
	case "$size" in size=0) problem "stress2 $t did nothing (size 0)";; esac
	[ $odd -eq 0 ] || problem "stress2 $t printed $odd unexpected lines (see s2-$t.log)"
done
left=$(ls -A "$X" 2>/dev/null | wc -l | tr -d ' ')
[ "$left" -eq 0 ] || problem "stressX not empty afterwards ($left entries)"
rm -rf "$W" "$X" 2>/dev/null

# --- after -----------------------------------------------------------------
{
	echo "== $(date -u) after (still mounted) =="; vmstat -m | grep -iE 'vboxvfs|iprtheap'; sysctl vfs.numvnodes vfs.freevnodes
	echo "== dmesg new lines =="; dmesg | tail -n +$((dmesg_before + 1))
} > "$LOGDIR/after.txt" 2>&1
newmsg=$(dmesg | tail -n +$((dmesg_before + 1)) | grep -ciE 'lock order|panic|witness|vboxvfs|sleeping thread|could sleep')
[ "$newmsg" -eq 0 ] || problem "$newmsg suspicious dmesg lines (see after.txt)"
if [ -z "${NO_UNMOUNT:-}" ]; then
	sync; sync
	umount "$MNT" && log "umount ok" || problem "umount failed"
	kldunload vboxvfs && log "kldunload ok" || problem "kldunload failed"
	{ echo "== after unmount/unload =="; vmstat -m | grep -iE 'vboxvfs|iprtheap' || echo "(no vboxvfs/iprtheap malloc type left)"; dmesg | tail -3; } >> "$LOGDIR/after.txt" 2>&1
	vmstat -m | grep -q ' vboxvfs ' && problem "vboxvfs malloc type still present after kldunload"
fi
log "done: $fail problem(s), $(( $(date +%s) - start )) s"
[ $fail -eq 0 ]
