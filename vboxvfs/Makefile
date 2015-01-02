# $FreeBSD$

VBOXROOT?=	${.CURDIR}/../../vboxwrksrc

KMOD=	vboxvfs

SRCS=	bus_if.h device_if.h vnode_if.h

SRCS+=	vboxvfs_prov.c
SRCS+=	vboxvfs_vfsops.c
SRCS+=	vboxvfs_vnops.c

CFLAGS+=	-DFREEBSD_STYLE \
		-I${VBOXROOT}/include \
		-I${VBOXROOT}/src/VBox/Additions

# Flags for VBox itself.
CFLAGS+=	-DDEBUG -DRT_OS_FREEBSD -D__FREEBSD__ -DRT_ARCH_AMD64 \
		-D__AMD64__ -DVBOX_WITH_HARDENING -DRT_LOCK_STRICT \
		-DRT_LOCK_STRICT_ORDER -DIN_RING0 -DIN_RT_R0 \
		-DVBOX -DVBOX_OSE -DVBOX_WITH_64_BITS_GUESTS \
		-D_KERNEL -DRTPATH_APP_PRIVATE=\"/usr/local/lib/virtualbox\" \
		-DRTPATH_APP_PRIVATE_ARCH=\"/usr/local/lib/virtualbox\" \
		-DRTPATH_SHARED_LIBS=\"/usr/local/lib/virtualbox\" \
		-DRTPATH_APP_DOCS=\"/usr/local/lib/virtualbox\" \
		-DHC_ARCH_BITS=64 -DGC_ARCH_BITS=64 -D_KERNEL -DKLD_MODULE \
		-DIN_GUEST -DIN_GUEST_R0 -DVBOX_WITH_HGCM

.include <bsd.kmod.mk>
