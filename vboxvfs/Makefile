# SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Development build of vboxvfs.ko with the FreeBSD kernel module framework
# (seconds instead of a kmk run, and the kernel's own compiler flags).
#
# It needs a VirtualBox source tree in which the guest additions have been
# built with kmk, e.g. the WRKSRC of emulators/virtualbox-ose-additions-72,
# for the headers and the two ring-0 guest libraries:
#
#	make VBOX_SRC=/usr/ports/emulators/virtualbox-ose-additions-72/work/VirtualBox-7.2.20
#
# VBOX_BUILD_TYPE selects out/freebsd.amd64/{debug,release}; SYSDIR must be
# the source tree of the running kernel (default /usr/src/sys).  The port
# builds the same sources through Makefile.kmk.

KMOD=	vboxvfs
SRCS=	vboxvfs_host.c vboxvfs_mount.c vboxvfs_node.c vboxvfs_vnode.c
SRCS+=	vnode_if.h

VBOX_SRC?=	/usr/ports/emulators/virtualbox-ose-additions-72/work/VirtualBox-7.2.20
VBOX_BUILD_TYPE?=	debug
VBOX_OUT?=	${VBOX_SRC}/out/freebsd.amd64/${VBOX_BUILD_TYPE}
VBOX_LIBDIR?=	${VBOX_OUT}/lib/additions
VBOX_LIBS?=	VBoxGuestR0Lib.a RuntimeGuestR0.a

CFLAGS+=	-I${VBOX_SRC}/include -I${VBOX_OUT}
CFLAGS+=	-DRT_OS_FREEBSD -DRT_ARCH_AMD64 -DRT_WITH_VBOX -DVBOX -DVBOX_OSE
CFLAGS+=	-DIN_RING0 -DIN_RT_R0 -DIN_GUEST -DIN_GUEST_R0 -DVBOX_WITH_HGCM
# The guest ring-0 libraries are built with mangled IPRT symbol names.
CFLAGS+=	-include ${VBOX_SRC}/include/VBox/VBoxGuestMangling.h
.if ${VBOX_BUILD_TYPE} == "debug"
CFLAGS+=	-DDEBUG -DRT_STRICT
.endif

# Kernel debugging options matching the development kernel (GENERIC on
# CURRENT); set WITHOUT_INVARIANTS for a release kernel.
.if !defined(WITHOUT_INVARIANTS)
CFLAGS+=	-DINVARIANTS -DINVARIANT_SUPPORT -DDEBUG_VFS_LOCKS
.endif

.include <bsd.kmod.mk>

# The VirtualBox headers do not survive the kernel's full warning set;
# these must come after kmod.mk's own warning flags.
CFLAGS+=	-Wno-undef -Wno-cast-qual -Wno-unused-parameter
CFLAGS+=	-Wno-missing-prototypes -Wno-missing-variable-declarations

# The static libraries are linked into the module.  Local copies are used
# because kmod.mk removes everything in OBJS on "make clean".
.for lib in ${VBOX_LIBS}
OBJS+=	${lib}
${lib}: ${VBOX_LIBDIR}/${lib}
	cp ${.ALLSRC} ${.TARGET}
.endfor
