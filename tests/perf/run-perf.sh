#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Host side: run bench.sh in a guest and collect the CSV.
# usage: run-perf.sh VM TAG DIR OUT [REMOUNT-command] [bench args...]
#   VM    ssh host alias
#   TAG   label (e.g. fbsd15-vboxvfs)
#   DIR   directory in the guest to benchmark in
#   OUT   host directory for results (created)
#   REMOUNT  guest command that drops the cache of DIR's fs ("" = none)
# env: TREE (guest copy of freebsd-vboxfs, default /root/freebsd-vboxfs),
#      BENCH_ENV extra "VAR=value" words passed to the bench
VM=${1:?VM}; TAG=${2:?tag}; DIR=${3:?dir}; OUT=${4:?out}; REMOUNT=${5:-}; shift 5 2>/dev/null || shift $#
TREE=${TREE:-/root/freebsd-vboxfs}; BENCH_ENV=${BENCH_ENV:-}
mkdir -p "$OUT"
ssh -o BatchMode=yes "$VM" "uname -a; nproc 2>/dev/null || sysctl -n hw.ncpu; mount | grep -E ' $(dirname $DIR)| $DIR' | head -3" > "$OUT/$TAG.env.txt" 2>&1
echo "$(date -u +%T) $TAG: start; host load: $(uptime | sed 's/.*load/load/')" >&2
ssh -o BatchMode=yes "$VM" "env REMOUNT='$REMOUNT' $BENCH_ENV sh $TREE/tests/perf/bench.sh $DIR $TAG $*" 2>"$OUT/$TAG.err" | LC_ALL=C tr -cd '\11\12\15\40-\176' | tee "$OUT/$TAG.csv"
echo "$(date -u +%T) $TAG: done; host load: $(uptime | sed 's/.*load/load/')" >&2
