#!/bin/sh
# Host-side check that a guest sees host-side changes within the attribute
# cache ttl: the host rewrites two files in a directory of the share every
# half second (one replaced atomically via rename, one rewritten in place)
# while the guest reads both every half second and reports the lag between
# the host's write and the guest's read.  Exit status 1 if the guest ever
# read a heartbeat older than the previous one or lagged more than
# TTLMS + 1000 ms.
#
# usage: coherence-check.sh VM [SECONDS] [TTLMS]
# env: SHARE (host dir of the share, default ~/vboxfs/share), MNT (guest
#      mount point, default /mnt/share)
VM=${1:?VM}; SECS=${2:-12}; TTLMS=${3:-200}
SHARE=${SHARE:-$HOME/vboxfs/share}; MNT=${MNT:-/mnt/share}
H=$SHARE/coherence-check; rm -rf "$H"; mkdir -p "$H" || exit 2
now_ms() { perl -MTime::HiRes=time -e 'printf "%d\n", time * 1000'; }
(
	i=0; end=$(( $(date +%s) + SECS ))
	while [ "$(date +%s)" -lt $end ]; do
		i=$((i + 1))
		echo "$i $(now_ms)" > "$H/.hb.tmp" && mv "$H/.hb.tmp" "$H/hb.txt"
		printf 'v%d %s\n' $i "$(now_ms)" > "$H/inplace.txt"
		sleep 0.5
	done
	echo "host: $i rounds"
) & writer=$!
ssh -o BatchMode=yes "$VM" "sleep 1; D=$MNT/coherence-check; last=0; max=0; maxip=0; bad=0; n=0
	end=\$(( \$(date +%s) + $SECS - 1 ))
	while [ \$(date +%s) -lt \$end ]; do
		now=\$(perl -MTime::HiRes=time -e 'printf \"%d\", time * 1000')
		set -- \$(cat \$D/hb.txt 2>/dev/null); hb=\${1:-0}; ts=\${2:-\$now}
		set -- \$(cat \$D/inplace.txt 2>/dev/null); ipts=\${2:-\$now}
		[ \$hb -lt \$last ] && { echo \"guest: heartbeat went backwards \$last -> \$hb\"; bad=\$((bad + 1)); }
		last=\$hb; n=\$((n + 1))
		lag=\$((now - ts)); [ \$lag -gt \$max ] && max=\$lag
		iplag=\$((now - ipts)); [ \$iplag -gt \$maxip ] && maxip=\$iplag
		sleep 0.5
	done
	echo \"guest: \$n reads, last heartbeat \$last, max lag rename=\${max}ms inplace=\${maxip}ms, errors \$bad\"
	[ \$bad -eq 0 ] && [ \$max -le $((TTLMS + 1000)) ] && [ \$maxip -le $((TTLMS + 1000)) ]"
rc=$?
wait $writer
rm -rf "$H"
exit $rc
