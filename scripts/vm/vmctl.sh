#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
#
# Start, stop and inspect the test guests.
#
# VBoxSVC is spawned by the first VBoxManage client and lives in that
# client's session; if the session belongs to a shell or tool that gets
# cleaned up, VBoxSVC and every running VM die with it.  "start" therefore
# runs VBoxManage through daemon(8), which gives it (and thus VBoxSVC) a
# session of its own.
#
# usage: vmctl.sh start|stop|poweroff|status|console vm...
#   start     start headless, detached
#   stop      ask the guest to shut down (ssh "shutdown -p now" for FreeBSD,
#             ACPI power button otherwise) and wait up to 3 minutes
#   poweroff  pull the plug (only for a guest that is already halted/hung)
#   status    VM state and whether ssh answers
#   console   tail the serial console log
#
# Environment: VBOXMANAGE (default "VBoxManage"; use "sudo -u $USER VBoxManage"
# when the vboxusers group is not yet active in this session), VM_DIR.
#
set -u
VBOXMANAGE=${VBOXMANAGE:-VBoxManage}
VM_DIR=${VM_DIR:-$HOME/vboxfs/vm}
LOGDIR=${LOGDIR:-$HOME/vboxfs/obj/logs}

state() {
	$VBOXMANAGE showvminfo "$1" --machinereadable 2>/dev/null |
	    sed -n 's/^VMState="\(.*\)"/\1/p'
}

cmd=${1:?usage: $0 start|stop|poweroff|status|console vm...}
shift
[ $# -gt 0 ] || { echo "$0: no VM given" >&2; exit 64; }
rc=0
for vm in "$@"; do
	case $cmd in
	start)
		mkdir -p "$LOGDIR"
		: > "$LOGDIR/startvm-$vm.log"
		# sudo -u is dropped by daemon(8)'s setsid; the environment
		# variable may carry it, so split VBOXMANAGE into the sudo
		# prefix and the command.
		case $VBOXMANAGE in
		sudo\ *)
			prefix=${VBOXMANAGE% VBoxManage}
			$prefix daemon -f -o "$LOGDIR/startvm-$vm.log" \
			    VBoxManage startvm "$vm" --type headless
			;;
		*)
			daemon -f -o "$LOGDIR/startvm-$vm.log" \
			    $VBOXMANAGE startvm "$vm" --type headless
			;;
		esac
		sleep 3
		cat "$LOGDIR/startvm-$vm.log"
		;;
	stop)
		if ssh -o ConnectTimeout=5 -o BatchMode=yes "$vm" \
		    'uname -s' 2>/dev/null | grep -q FreeBSD; then
			ssh -o ConnectTimeout=5 -o BatchMode=yes "$vm" \
			    'sync; shutdown -p now' >/dev/null 2>&1
		else
			$VBOXMANAGE controlvm "$vm" acpipowerbutton
		fi
		# A FreeBSD guest under EFI halts ("All buffers synced") instead
		# of powering off; pull the plug once it says so.
		n=0
		while [ "$(state "$vm")" != "poweroff" ] && [ $n -lt 36 ]; do
			sleep 5
			n=$((n + 1))
			if tail -5 "$VM_DIR/$vm-console.log" 2>/dev/null |
			    grep -qE "All buffers synced|has halted|press any key"; then
				sleep 3
				$VBOXMANAGE controlvm "$vm" poweroff 2>/dev/null
				sleep 3
			fi
		done
		echo "$vm: $(state "$vm") after ~$((n * 5))s"
		[ "$(state "$vm")" = "poweroff" ] || rc=1
		;;
	poweroff)
		$VBOXMANAGE controlvm "$vm" poweroff
		;;
	status)
		printf '%s: %s' "$vm" "$(state "$vm")"
		if ssh -o ConnectTimeout=3 -o BatchMode=yes "$vm" true \
		    2>/dev/null; then
			printf ', ssh ok\n'
		else
			printf ', no ssh\n'
		fi
		;;
	console)
		tail -20 "$VM_DIR/$vm-console.log"
		;;
	*)
		echo "$0: unknown command $cmd" >&2
		exit 64
		;;
	esac
done
exit $rc
