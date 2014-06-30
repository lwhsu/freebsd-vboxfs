freebsd-vboxfs
==============

To setup development environment quickly:
```sh
cd /usr/ports/emulators/virtualbox-ose-additions
(optional) make config; unselect "X11"
make all-depends-list | awk -F/ '{print $5}' > /tmp/vbox-addon-depends
pkg install `cat /tmp/vbox-addon-depends`
edit /tmp/vbox-addon-depends, remove/modify error entries from last command:
  perl5.16 -> perl5
  gcc-ecj45 -> gcc-ecj
  delete indexinfo
pkg install `cat /tmp/vbox-addon-depends`
```

To build:

```sh
cp -R $(freebsd-vboxsf)/mount_vboxfs /usr/src/sbin
cd /usr/src/sbin/mount_vboxfs && make depend all install

cp $(freebsd-vboxsf)/patch-src-VBox-Additions-freebsd-Makefile.kmk /usr/ports/emulators/virtualbox-ose/files

cd /usr/ports/emulators/virtualbox-ose-additions
make patch
rm -fr `make -V WRKSRC`/src/VBox/Additions/freebsd/vboxvfs
cp -R $(freebsd-vboxsf)/vboxvfs `make -V WRKSRC`/src/VBox/Additions/freebsd

make
```

To test: (currently does not work)
```sh
cd /usr/ports/emulators/virtualbox-ose-additions
cd `make -V WRKSRC`
kldload ./VirtualBox-4.3.12/out/freebsd.amd64/release/bin/additions/vboxguest.ko
kldload ./VirtualBox-4.3.12/out/freebsd.amd64/release/bin/additions/vboxvfs.ko

mount_vboxfs -w shared_folder_name /mnt
```
