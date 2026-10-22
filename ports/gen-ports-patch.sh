#!/bin/sh
# Turn this tree into the FreeBSD ports changes that ship it: remove the
# old driver's patches from the shared PATCHDIR of the virtualbox-ose-72
# ports, add ours (new-file patches for the sources, a real diff for the
# vboxvfs Makefile.kmk), and teach emulators/virtualbox-ose-additions-72 to
# install the manual page.  Idempotent: run it again after changing the
# sources and only the patches that differ change.
#
# usage: gen-ports-patch.sh PORTSDIR [VBOXSRC]
#   PORTSDIR  a ports tree checkout (the branch to commit on)
#   VBOXSRC   pristine VirtualBox source of the version the ports build
#             (default ~/vboxfs/virtualbox), for the Makefile.kmk diff
set -eu
PORTSDIR=${1:?ports tree}; VBOXSRC=${2:-$HOME/vboxfs/virtualbox}
HERE=$(cd "$(dirname "$0")/.." && pwd)
SH=$PORTSDIR/emulators/virtualbox-ose-72/files
A=$PORTSDIR/emulators/virtualbox-ose-additions-72
ADD=src/VBox/Additions/freebsd
[ -d "$SH" ] && [ -f "$A/Makefile" ] || { echo "$PORTSDIR: not a ports tree with virtualbox-ose-72" >&2; exit 1; }
[ -f "$VBOXSRC/$ADD/vboxvfs/Makefile.kmk" ] || { echo "$VBOXSRC: no pristine $ADD/vboxvfs/Makefile.kmk" >&2; exit 1; }
stamp=$(date -u '+%Y-%m-%d %H:%M:%S UTC')

# patch-<path with _ doubled and / turned into _> (Mk/Scripts/smart_makepatch.sh)
patchname() { echo "patch-$(echo "$1" | sed -e 's/_/__/g' -e 's|/|_|g')"; }

# write a new-file patch for $2 (path in the source tree) from $1 (our file)
newfile_patch() {
	src=$1; path=$2; out=$SH/$(patchname "$path"); tmp=$out.new
	n=$(wc -l < "$src" | tr -d ' ')
	{
		printf -- '--- %s.orig\t%s\n+++ %s\n@@ -0,0 +1,%s @@\n' "$path" "$stamp" "$path" "$n"
		sed -e 's/^/+/' "$src"
	} > "$tmp"
	install_if_changed "$tmp" "$out"
}
# keep the old file (and its timestamp line) when only the timestamp differs
install_if_changed() {
	tmp=$1; out=$2
	if [ -f "$out" ]; then sed 1d "$tmp" > "$tmp.a"; sed 1d "$out" > "$tmp.b"; fi
	if [ -f "$out" ] && cmp -s "$tmp.a" "$tmp.b"; then rm -f "$tmp"; echo "  unchanged $(basename "$out")"
	else mv "$tmp" "$out"; echo "  wrote     $(basename "$out")"; fi
	rm -f "$tmp.a" "$tmp.b"
}

echo "== removing the old driver's patches"
# (the mount_vboxvfs and vboxvfs/Makefile.kmk patches keep their names and are
# overwritten below)
for f in Makefile vboxvfs_bcmp.c vboxvfs_vboxvfs__prov.c vboxvfs_vboxvfs__vfsops.c \
    vboxvfs_vboxvfs__vnops.c vboxvfs_vboxvfs.h; do
	p=$SH/patch-src_VBox_Additions_freebsd_$f
	[ -f "$p" ] && { rm -f "$p"; echo "  removed   $(basename "$p")"; } || true
done

echo "== new-file patches for the rewrite"
for f in vboxvfs_compat.c vboxvfs_host.c vboxvfs_internal.h vboxvfs_io.c \
    vboxvfs_mount.c vboxvfs_node.c vboxvfs_vnode.c; do
	newfile_patch "$HERE/vboxvfs/$f" "$ADD/vboxvfs/$f"
done
for f in mount_vboxvfs.c mount_vboxvfs.8 Makefile.kmk; do
	newfile_patch "$HERE/mount_vboxvfs/$f" "$ADD/mount_vboxvfs/$f"
done

echo "== vboxvfs/Makefile.kmk as a diff against upstream"
path=$ADD/vboxvfs/Makefile.kmk; out=$SH/$(patchname "$path"); tmp=$out.new
{
	printf -- '--- %s.orig\t%s\n+++ %s\n' "$path" "$stamp" "$path"
	diff -u "$VBOXSRC/$path" "$HERE/vboxvfs/Makefile.kmk" | sed -e '1,2d'
} > "$tmp"
install_if_changed "$tmp" "$out"

echo "== emulators/virtualbox-ose-additions-72: PORTREVISION, manual page"
if ! grep -q '^PORTREVISION=' "$A/Makefile"; then
	sed -i '' '/^DISTVERSION=/a\
PORTREVISION=	1
' "$A/Makefile"; echo "  added PORTREVISION"
fi
if ! grep -q 'mount_vboxvfs.8' "$A/Makefile"; then
	# after the INSTALL_PROGRAM ${VBOX_SBINS...} line and its continuation
	ln=$(grep -n 'INSTALL_PROGRAM} ${VBOX_SBINS' "$A/Makefile" | cut -d: -f1)
	[ -n "$ln" ] || { echo "cannot find the VBOX_SBINS install line" >&2; exit 1; }
	sed -i '' "$((ln + 1))a\\
	\${MKDIR} \${STAGEDIR}\${PREFIX}/share/man/man8\\
	\${INSTALL_MAN} \${VBOX_BIN}/mount_vboxvfs.8 \\\\\\
	    \${STAGEDIR}\${PREFIX}/share/man/man8/
" "$A/Makefile"; echo "  added man page install"
fi
if ! grep -q 'share/man/man8/mount_vboxvfs.8.gz' "$A/pkg-plist"; then
	sed -i '' '/^%%X11%%share\/autostart\/vboxclient.desktop$/a\
share/man/man8/mount_vboxvfs.8.gz
' "$A/pkg-plist"; echo "  added plist entry"
fi
if ! grep -q 'mount_vboxvfs' "$A/pkg-message"; then
	# a short note on shared folders after the "restart the system" line
	ln=$(grep -n '^To start the services, restart the system.$' "$A/pkg-message" | cut -d: -f1)
	[ -n "$ln" ] || { echo "cannot find the anchor line in pkg-message" >&2; exit 1; }
	sed -i '' "${ln}a\\
\\
Shared folders are mounted with mount_vboxvfs(8); to mount the folder\\
named \"share\" so that user 1001 owns the files:\\
\\
# mount -t vboxvfs -o uid=1001,gid=1001 share /mnt/share\\
\\
or in /etc/fstab:\\
\\
share /mnt/share vboxvfs rw,uid=1001,gid=1001,late 0 0
" "$A/pkg-message"; echo "  added pkg-message note"
fi
echo "== result"
git -C "$PORTSDIR" status --short -- emulators/virtualbox-ose-72/files emulators/virtualbox-ose-additions-72 | sed 's/^/  /'
