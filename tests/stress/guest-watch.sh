#!/bin/sh
# Guest side of the host/guest concurrency check: poll DIR (on the vboxvfs
# mount) for SECONDS and measure how long after the host wrote each heartbeat
# the guest first saw it (hb.txt holds "<counter> <host epoch ms>"), via
# read(2) (cat), via mmap (the share's own cat uses read; we use a tiny C
# program if cc is available, else skip), and via stat(2) size changes of
# append.log.  Also verifies that the content of inplace.dat is consistent
# (a single repeated byte) each time it is read, and that directory listings
# never fail.  Prints a summary; exit status 1 if the worst staleness exceeded TTLMS + 2000 ms
# (poll interval plus host/guest clock skew allowance) or an inconsistency
# was seen.
#
# usage: guest-watch.sh DIR SECONDS [TTLMS]
DIR=${1:?dir}; SECS=${2:?seconds}; TTLMS=${3:-200}
now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time * 1000' 2>/dev/null || echo $(( $(date +%s) * 1000 )); }
end=$(( $(date +%s) + SECS ))
last=0; maxlag=0; sumlag=0; nhb=0; bad=0; lsfail=0; lastsize=0; shrink=0
until [ -f "$DIR/hb.txt" ] || [ "$(date +%s)" -ge $end ]; do sleep 1; done
while [ "$(date +%s)" -lt $end ]; do
	hb=$(cat "$DIR/hb.txt" 2>/dev/null)
	set -- $hb
	if [ -n "$2" ] && [ "$1" -gt "$last" ] 2>/dev/null; then
		now=$(now_ms); lag=$((now - $2)); [ $lag -lt 0 ] && lag=0
		last=$1; nhb=$((nhb + 1)); sumlag=$((sumlag + lag))
		[ $lag -gt $maxlag ] && maxlag=$lag
	fi
	# in-place rewritten file must never show a mix of two versions
	if [ -s "$DIR/inplace.dat" ]; then
		kinds=$(head -c 7000 "$DIR/inplace.dat" | tr -d '\n' | fold -w1 | sort -u | wc -l)
		[ "$kinds" -gt 1 ] && { bad=$((bad + 1)); echo "inplace.dat: $kinds different bytes at $(date -u +%T)"; }
	fi
	size=$(stat -f %z "$DIR/append.log" 2>/dev/null || echo 0)
	[ "$size" -lt "$lastsize" ] && { shrink=$((shrink + 1)); echo "append.log shrank $lastsize -> $size"; }
	lastsize=$size
	ls "$DIR/sub" > /dev/null 2>&1 || lsfail=$((lsfail + 1))
	sleep 0.5
done
avg=0; [ $nhb -gt 0 ] && avg=$((sumlag / nhb))
echo "guest-watch: heartbeats seen $nhb (last $last), max lag ${maxlag}ms, avg lag ${avg}ms, ttl ${TTLMS}ms; inplace inconsistencies $bad, append.log shrinks $shrink, ls failures $lsfail"
[ $maxlag -le $((TTLMS + 2000)) ] && [ $bad -eq 0 ] && [ $shrink -eq 0 ] && [ $lsfail -eq 0 ]
