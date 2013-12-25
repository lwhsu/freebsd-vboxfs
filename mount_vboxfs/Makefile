PROG= mount_vboxfs
SRCS= mount_vboxfs.c getmntopts.c
MAN= mount_vboxfs.8

MOUNT= ${.CURDIR}/../mount
CFLAGS+=-I${MOUNT}

.PATH: ${MOUNT}

.include <bsd.prog.mk>

