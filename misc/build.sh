#/bin/sh

rm -f \
	work/.build_done.virtualbox-ose._usr_local \
	work/.stage_done.virtualbox-ose._usr_local
rm -fr work/VirtualBox-4.3.20/out
make
