#!/bin/sh
#
# SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Copy the working tree to a test guest (default: fbsd16, see ~/.ssh/config)
# into ~/freebsd-vboxfs there.  The source of truth stays on the host; the
# guest only builds and loads.
#
# usage: sync-guest.sh [guest]
#
set -eu
GUEST=${1:-fbsd16}
TOP=$(cd "$(dirname "$0")/../.." && pwd)
exec rsync -a --delete --exclude .git --exclude '*.o' --exclude '*.ko' \
    --exclude '*.ko.debug' --exclude '*.ko.full' --exclude '.depend*' \
    --exclude machine --exclude x86 --exclude i386 \
    --exclude mount_vboxvfs/mount_vboxvfs --exclude '*.full' --exclude '*.8.gz' \
    "$TOP/" "$GUEST:freebsd-vboxfs/"
