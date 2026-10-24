# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Stand-alone build with the FreeBSD bsd.prog.mk framework (development and
# packaging outside the VirtualBox tree).  The VirtualBox tree builds the
# same source through Makefile.kmk.

PROG=	mount_vboxvfs
MAN=	mount_vboxvfs.8
WARNS?=	6
LDADD=	-lutil

BINDIR?=	/usr/local/sbin
MANDIR?=	/usr/local/share/man/man

.include <bsd.prog.mk>
