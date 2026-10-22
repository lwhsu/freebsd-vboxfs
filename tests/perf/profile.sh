#!/bin/sh
# Run a few of the bench workloads one at a time under DTrace to count the
# host requests vboxvfs makes for each and how long they take (hostreqs.d).
# FreeBSD guest only; needs the module loaded and DIR on the share.
# usage: profile.sh DIR OUTDIR [SIZE_MB]
set -u
DIR=${1:?dir}; OUTDIR=${2:?outdir}; SIZE_MB=${3:-256}
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$DIR" "$OUTDIR"
run() {	# $1 = name, $2 = shell command
	name=$1; shift
	echo "== $name"
	dtrace -Z -s "$HERE/hostreqs.d" -c "sh -c '$*'" > "$OUTDIR/$name.dtrace" 2>"$OUTDIR/$name.err"
	grep -A20 '== host requests ==' "$OUTDIR/$name.dtrace" | grep -vE '^$|==' | awk '{ printf "   %-22s %s\n", $1, $2 }'
	grep -A3 '== bytes ==' "$OUTDIR/$name.dtrace" | grep -E 'read|write' | sed 's/^/   /'
}
big=$DIR/big; rm -f $big
run seqwrite_1m "dd if=/dev/zero of=$big bs=1048576 count=$SIZE_MB conv=fsync 2>/dev/null"
rm -f $big
run seqwrite_4k "dd if=/dev/zero of=$big bs=4096 count=$((SIZE_MB * 256 / 4)) conv=fsync 2>/dev/null"
rm -f $big; dd if=/dev/zero of=$big bs=1048576 count=$SIZE_MB conv=fsync 2>/dev/null
run overwrite_1m "dd if=/dev/zero of=$big bs=1048576 count=$SIZE_MB conv=notrunc,fsync 2>/dev/null"
sh -c "${REMOUNT:-true}" >/dev/null 2>&1
run seqread_cold_1m "dd if=$big of=/dev/null bs=1048576 2>/dev/null"
run seqread_warm_1m "dd if=$big of=/dev/null bs=1048576 2>/dev/null"
sh -c "${REMOUNT:-true}" >/dev/null 2>&1
run fio_randread_4k_qd1 "fio --name=rr --filename=$big --size=${SIZE_MB}M --runtime=10 --time_based --direct=0 --rw=randread --bs=4k --ioengine=psync --numjobs=1 --output=/dev/null"
run fio_randwrite_4k_qd1 "fio --name=rw --filename=$big --size=${SIZE_MB}M --runtime=10 --time_based --direct=0 --rw=randwrite --bs=4k --ioengine=psync --numjobs=1 --output=/dev/null"
rm -f $big
m=$DIR/meta
run create_1k "i=0; while [ \$i -lt 10 ]; do d=$m/d\$i; mkdir -p \$d; j=0; while [ \$j -lt 100 ]; do printf '%4096s' x > \$d/f\$j; j=\$((j + 1)); done; i=\$((i + 1)); done"
run stat_1k_warm "find $m -ls > /dev/null"
sh -c "${REMOUNT:-true}" >/dev/null 2>&1
run stat_1k_cold "find $m -ls > /dev/null"
run read_1k_small "find $m -type f | xargs cat > /dev/null"
run rm_1k "rm -rf $m"
run tar_extract "tar xf ${TREE_TAR:-/root/perf-tree.tar} -C $DIR"
sh -c "${REMOUNT:-true}" >/dev/null 2>&1
run tar_readback_cold "tar cf /dev/null -C $DIR tree"
run rm_tree "rm -rf $DIR/tree"
rm -rf "$DIR"/* 2>/dev/null
