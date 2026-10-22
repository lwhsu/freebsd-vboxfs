#!/bin/sh
# Run inside the guest: environment description for the results README.
echo "== guest =="; uname -a; sysctl -n kern.osreldate; sysctl kern.conftxt | grep -E 'INVARIANTS|WITNESS|DEBUG_VFS_LOCKS|^ident' || echo "(no INVARIANTS/WITNESS/DEBUG_VFS_LOCKS in kernel config)"
echo "== additions pkg / modules =="; pkg info -x virtualbox 2>/dev/null; kldstat; ls -la /boot/modules/
echo "== rewrite tree =="; cd /root/freebsd-vboxfs 2>/dev/null && ls -la vboxvfs/vboxvfs.ko mount_vboxvfs/mount_vboxvfs 2>/dev/null; md5 vboxvfs/vboxvfs.ko 2>/dev/null; nm vboxvfs/vboxvfs.ko 2>/dev/null | grep -c ' U '
echo "== src/ports =="; git -C /usr/src log --oneline -1 2>/dev/null; git -C /usr/ports log --oneline -1 2>/dev/null; grep -E '^#define __FreeBSD_version' /usr/src/sys/sys/param.h 2>/dev/null
echo "== WRKSRC =="; cd /usr/ports/emulators/virtualbox-ose-additions-72 2>/dev/null && make FLAVOR=xorg -V WRKSRC -V PKGNAME 2>/dev/null
echo "== mount =="; mount -p | grep vboxvfs; echo "vboxvfs mounts: $(mount -p | grep -c vboxvfs)"; mount -v | grep vboxvfs; stat -f '%Sp %u:%g' /mnt/share 2>/dev/null; sysctl vfs.vboxvfs 2>/dev/null
echo "== pjdfstest =="; cd /root/pjdfstest && git log --oneline -1 && git status --short | head -20; cd /mnt/share/pjd-fbsd15 2>/dev/null && sh -c '. /root/pjdfstest/tests/conf; echo "os=$os fs=$fs"'
echo "== date =="; date -u
