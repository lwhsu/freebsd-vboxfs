freebsd-vboxfs
==============

To build:

```sh
cp -R $(freebsd-vboxsf)/mount_vboxfs /usr/src/sbin
cd /usr/src/sbin/mount_vboxfs && make install

cp $(freebsd-vboxsf)/patch-src-VBox-Additions-freebsd-Makefile.kmk /usr/ports/emulators/virtualbox-ose-additions/files

cd /usr/ports/emulators/virtualbox-ose-additions
make patch
rm -fr `make -V WRKSRC`/src/VBox/Additions/freebsd/vboxvfs
cp -R $(freebsd-vboxsf)/vboxvfs `make -V WRKSRC`/src/VBox/Additions/freebsd

make install
```
