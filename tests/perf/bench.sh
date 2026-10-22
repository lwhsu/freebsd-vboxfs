#!/bin/sh
# Portable (FreeBSD sh / dash) file system benchmark used to compare the
# vboxvfs guest driver with the Linux vboxsf client and with local disks.
# Runs sequential dd, fio (psync and mmap engines), a metadata set (10000
# small files, one flat directory of 10000 entries), and a source-tree
# round trip (tar extract, tar read-back, diff -r, rm -rf).  Prints CSV
# lines "tag,test,value,unit" on stdout and keeps the raw fio output in
# $OUTDIR.
#
# usage: bench.sh DIR TAG [SIZE_MB]
#   DIR      directory to benchmark in (created; everything inside is removed)
#   TAG      label for the CSV lines
#   SIZE_MB  size of the sequential dd files (default 1024)
# env: REMOUNT  command that drops the guest's cache of DIR's file system
#               (typically "umount X && mount ..."); run between phases that
#               must start cold; empty = no cold phases
#      TREE_TAR tarball with a source tree (top directory "tree") on local
#               disk, for the tar/diff tests (default /root/perf-tree.tar)
#      LOCAL_TREE directory on local disk where the same tree is extracted
#               for diff -r (default /root/perf-tree)
#      FIO_RUNTIME seconds per fio job (default 20); FIO_SIZE (default 512M)
#      OUTDIR   where fio output goes (default /root/perf-out)
#      SKIP     space separated test group names to skip: dd fio meta tree
set -u
DIR=${1:?dir}; TAG=${2:?tag}; SIZE_MB=${3:-1024}
REMOUNT=${REMOUNT:-}; TREE_TAR=${TREE_TAR:-/root/perf-tree.tar}; LOCAL_TREE=${LOCAL_TREE:-/root/perf-tree}
FIO_RUNTIME=${FIO_RUNTIME:-20}; FIO_SIZE=${FIO_SIZE:-512M}; OUTDIR=${OUTDIR:-/root/perf-out}; SKIP=${SKIP:-}
mkdir -p "$DIR" "$OUTDIR"
cd /
now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
skip() { case " $SKIP " in *" $1 "*) return 0;; esac; return 1; }
emit() { echo "$TAG,$1,$2,$3"; }
# time a command; emit seconds, and MB/s if $MB is set
t0=0
tstart() { t0=$(now); }
tend() {	# $1 = test name, $2 = MB moved (optional)
	t1=$(now); secs=$(echo "$t1 $t0" | awk '{ printf "%.3f", $1 - $2 }')
	emit "$1" "$secs" s
	[ -n "${2:-}" ] && emit "$1" "$(echo "$2 $secs" | awk '{ if ($2 > 0) printf "%.1f", $1 / $2; else print 0 }')" MB/s
	return 0
}
drop() { [ -n "$REMOUNT" ] && { sync; sh -c "$REMOUNT" >/dev/null 2>&1 || echo "$TAG,remount,FAILED,-"; }; return 0; }
rm -rf "$DIR"/* 2>/dev/null

if ! skip dd; then
	big=$DIR/big
	tstart; dd if=/dev/zero of=$big bs=1048576 count=$SIZE_MB conv=fsync 2>/dev/null; tend seqwrite_1m $SIZE_MB
	rm -f $big
	tstart; dd if=/dev/zero of=$big bs=65536 count=$((SIZE_MB * 16)) conv=fsync 2>/dev/null; tend seqwrite_64k $SIZE_MB
	rm -f $big
	q=$((SIZE_MB / 4))
	tstart; dd if=/dev/zero of=$big bs=4096 count=$((q * 256)) conv=fsync 2>/dev/null; tend seqwrite_4k $q
	rm -f $big
	dd if=/dev/zero of=$big bs=1048576 count=$SIZE_MB conv=fsync 2>/dev/null
	drop
	tstart; dd if=$big of=/dev/null bs=1048576 2>/dev/null; tend seqread_cold_1m $SIZE_MB
	tstart; dd if=$big of=/dev/null bs=1048576 2>/dev/null; tend seqread_warm_1m $SIZE_MB
	drop
	tstart; dd if=$big of=/dev/null bs=4096 2>/dev/null; tend seqread_cold_4k $SIZE_MB
	# overwrite in place (no truncate): write path without extension
	tstart; dd if=/dev/zero of=$big bs=1048576 count=$SIZE_MB conv=notrunc,fsync 2>/dev/null; tend overwrite_1m $SIZE_MB
	rm -f $big
fi

if ! skip fio; then
	f=$DIR/fiotest
	fiorun() {	# $1 = name, rest = fio args; emits bw and iops of the given direction
		name=$1; shift
		fio --name=$name --filename=$f --size=$FIO_SIZE --runtime=$FIO_RUNTIME --time_based \
		    --direct=0 --group_reporting --output-format=terse --terse-version=3 "$@" > $OUTDIR/$name.terse 2>$OUTDIR/$name.err || echo "$TAG,$name,FAILED,-"
		# terse v3: read bw=$7 KiB/s iops=$8; write bw=$48 iops=$49
		awk -F';' -v tag="$TAG" -v n="$name" 'NR == 1 { if ($7 + 0 > 0) { printf "%s,%s_read,%.1f,MB/s\n%s,%s_read,%.0f,iops\n", tag, n, $7 / 1024, tag, n, $8 }
		    if ($48 + 0 > 0) { printf "%s,%s_write,%.1f,MB/s\n%s,%s_write,%.0f,iops\n", tag, n, $48 / 1024, tag, n, $49 } }' $OUTDIR/$name.terse
	}
	dd if=/dev/zero of=$f bs=1048576 count=512 conv=fsync 2>/dev/null
	drop
	fiorun fio_randread_4k_qd1 --rw=randread --bs=4k --ioengine=psync --numjobs=1
	fiorun fio_randread_4k_j4 --rw=randread --bs=4k --ioengine=psync --numjobs=4
	fiorun fio_randwrite_4k_qd1 --rw=randwrite --bs=4k --ioengine=psync --numjobs=1
	fiorun fio_randwrite_4k_j4 --rw=randwrite --bs=4k --ioengine=psync --numjobs=4
	fiorun fio_randrw_4k_j4 --rw=randrw --bs=4k --ioengine=psync --numjobs=4
	drop
	fiorun fio_seqread_1m --rw=read --bs=1m --ioengine=psync --numjobs=1
	fiorun fio_seqwrite_1m --rw=write --bs=1m --ioengine=psync --numjobs=1
	drop
	fiorun fio_mmap_randread_4k --rw=randread --bs=4k --ioengine=mmap --numjobs=1
	fiorun fio_mmap_randwrite_4k --rw=randwrite --bs=4k --ioengine=mmap --numjobs=1
	rm -f $f
fi

if ! skip meta; then
	m=$DIR/meta
	tstart
	i=0; while [ $i -lt 100 ]; do d=$m/d$i; mkdir -p $d; j=0; while [ $j -lt 100 ]; do printf '%4096s' x > $d/f$j; j=$((j + 1)); done; i=$((i + 1)); done
	tend create_10k
	tstart; find $m -ls > /dev/null; tend stat_10k
	drop
	tstart; find $m -ls > /dev/null; tend stat_10k_cold
	tstart; find $m -type f | xargs cat > /dev/null; tend read_10k_small
	tstart; rm -rf $m; tend rm_10k
	flat=$DIR/flat; mkdir -p $flat
	tstart; i=0; while [ $i -lt 10000 ]; do : > $flat/f$i; i=$((i + 1)); done; tend create_10k_flat
	tstart; ls -f $flat > /dev/null; tend readdir_10k_flat
	drop
	tstart; ls -l $flat > /dev/null; tend ls_l_10k_flat_cold
	tstart; ls -l $flat > /dev/null; tend ls_l_10k_flat_warm
	tstart; rm -rf $flat; tend rm_10k_flat
fi

if ! skip tree && [ -f "$TREE_TAR" ]; then
	tstart; tar xf $TREE_TAR -C $DIR; tend tar_extract
	drop
	tstart; tar cf /dev/null -C $DIR tree; tend tar_readback_cold
	tstart; tar cf /dev/null -C $DIR tree; tend tar_readback_warm
	[ -d "$LOCAL_TREE/tree" ] && { tstart; diff -r $LOCAL_TREE/tree $DIR/tree > /dev/null; tend diff_r_tree; }
	tstart; find $DIR/tree -type f -name '*.c' | xargs grep -l vnode > /dev/null; tend grep_tree
	tstart; rm -rf $DIR/tree; tend rm_tree
fi
rm -rf "$DIR"/* 2>/dev/null
emit done 1 -
