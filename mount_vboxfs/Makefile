BINDIR?=	/sbin

PROG=		mount_vboxfs
SRCS=		getmntopts.c \
		mount_vboxfs.c
MAN=		mount_vboxfs.8

FREEBSD_SRC?=	${.CURDIR}/../../freebsd
.if !exists(${FREEBSD_SRC})
FREEBSD_SRC?=	/usr/src
.endif
MOUNT=		${FREEBSD_SRC}/sbin/mount

CFLAGS+=-I${MOUNT}

.PATH: ${MOUNT}

.include <bsd.prog.mk>
