#!/bin/sh
#
# SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Create one of the test guests (fbsd16, fbsd15, linux) from a prepared
# disk image.  NAT networking with ssh forwarded from 127.0.0.1 only, a
# serial console written to a file, one shared folder named "share" with
# symlink creation enabled.
#
# usage: create-vm.sh fbsd16|fbsd15|linux
#
# Environment:
#   VBOXMANAGE  command used to run VBoxManage (default: VBoxManage;
#               e.g. "sudo -u $USER VBoxManage" until the vboxusers group
#               membership is active in the current session)
#   VM_DIR      VirtualBox base folder (default: ~/vboxfs/vm)
#   SHARE_DIR   host directory of the shared folder (default: ~/vboxfs/share)
#   IMG_DIR     where the source images are (default: $VM_DIR/images)
#
set -eu

VM=${1:?usage: $0 fbsd16|fbsd15|linux}
VBOXMANAGE=${VBOXMANAGE:-VBoxManage}
VM_DIR=${VM_DIR:-$HOME/vboxfs/vm}
SHARE_DIR=${SHARE_DIR:-$HOME/vboxfs/share}
IMG_DIR=${IMG_DIR:-$VM_DIR/images}

SEED=
case $VM in
fbsd16)
	OSTYPE=FreeBSD_64; CPUS=4; MEM=8192; DISK_MB=40960; PORT=2216
	FIRMWARE=efi
	RAW=$IMG_DIR/FreeBSD-16.0-CURRENT-amd64-ufs.raw
	;;
fbsd15)
	OSTYPE=FreeBSD_64; CPUS=2; MEM=4096; DISK_MB=40960; PORT=2215
	FIRMWARE=efi
	RAW=$IMG_DIR/FreeBSD-15.1-RELEASE-amd64-ufs.raw
	;;
linux)
	OSTYPE=Ubuntu24_LTS_64; CPUS=2; MEM=4096; DISK_MB=20480; PORT=2222
	FIRMWARE=bios
	RAW=	# linux.vdi is produced by clonemedium from the cloud image
	SEED=$VM_DIR/seed-linux.iso
	;;
*)
	echo "$0: unknown VM $VM" >&2; exit 64 ;;
esac

VDI=$VM_DIR/$VM.vdi
mkdir -p "$VM_DIR" "$SHARE_DIR"

if [ ! -f "$VDI" ]; then
	if [ -z "$RAW" ]; then
		echo "$0: $VDI missing; create it with VBoxManage clonemedium first" >&2
		exit 1
	fi
	$VBOXMANAGE convertfromraw "$RAW" "$VDI" --format VDI
fi
$VBOXMANAGE modifymedium disk "$VDI" --resize "$DISK_MB"

$VBOXMANAGE createvm --name "$VM" --ostype "$OSTYPE" --register --basefolder "$VM_DIR"
$VBOXMANAGE modifyvm "$VM" --cpus "$CPUS" --memory "$MEM" --vram 16 \
    --firmware "$FIRMWARE" --graphicscontroller vmsvga \
    --audio-enabled off --usb-ohci off --mouse ps2 \
    --nic1 nat --nat-pf1 "ssh,tcp,127.0.0.1,$PORT,,22" --nat-localhostreachable1 on \
    --uart1 0x3F8 4 --uart-mode1 file "$VM_DIR/$VM-console.log"
$VBOXMANAGE storagectl "$VM" --name SATA --add sata --controller IntelAhci --portcount 4
$VBOXMANAGE storageattach "$VM" --storagectl SATA --port 0 --device 0 --type hdd --medium "$VDI"
if [ -n "$SEED" ]; then
	$VBOXMANAGE storageattach "$VM" --storagectl SATA --port 1 --device 0 \
	    --type dvddrive --medium "$SEED"
fi
$VBOXMANAGE sharedfolder add "$VM" --name share --hostpath "$SHARE_DIR"
$VBOXMANAGE setextradata "$VM" VBoxInternal2/SharedFoldersEnableSymlinksCreate/share 1

$VBOXMANAGE showvminfo "$VM" | grep -E '^(Name|Guest OS|Memory size|Number of CPUs|Firmware|NIC 1 Rule|UART 1|Shared folders|Name:.*share)' || true
echo "$VM created; start with: $VBOXMANAGE startvm $VM --type headless"
