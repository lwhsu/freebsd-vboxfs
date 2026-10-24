# freebsd-vboxfs

A rewrite of the VirtualBox shared folder client for FreeBSD guests:
the `vboxvfs` kernel module and the `mount_vboxvfs(8)` utility.

The code in this branch (`rewrite`) is new work under the BSD 2-Clause
license (`BSD-2-Clause`, see `LICENSE`).  It does not derive from the 2016
Solaris-based port that lives on the `master` branch and in the FreeBSD
ports tree (`emulators/virtualbox-ose-additions-72`); that code is kept only
for history.  The one file that is not ours is `vboxvfs/Makefile.kmk`:
Oracle's kBuild file for the module with a few changes, which keeps
Oracle's GPL-3.0-only license.

## Layout

| Directory        | Contents |
|------------------|----------|
| `vboxvfs/`       | kernel module sources (`Makefile` for `bsd.kmod.mk`, `Makefile.kmk` for the VirtualBox kBuild tree) |
| `mount_vboxvfs/` | `mount_vboxvfs(8)` and its manual page |
| `ports/`         | `gen-ports-patch.sh` turns this tree into the FreeBSD ports change; `COMMITMSG` is the commit message for it |
| `scripts/vm/`    | VM provisioning (`prep-freebsd-image.sh`, `create-vm.sh`, `vmctl.sh`) |
| `scripts/dev/`   | `sync-guest.sh` (rsync the tree into a guest), `gen-opt-global.sh` (kernel options for development builds) |
| `tests/`         | `smoke-*.sh`, `writebehind-check.sh` (+ `writebehind-guest.pl`: what the host sees of data written behind), the pjdfstest local patch, `pjd/` (pjdfstest runner and analysis), `stress/` (10 minute stress run, host/guest coherence check), `perf/` (benchmark, DTrace host request profile, result tables) |

## Design in one paragraph

Nodes are cached by (parent, name) with `vfs_hash(9)` and get inode numbers
from a hash of the path; attributes are cached for a ttl (mount option `ttl`,
default 200 ms) after which the host is asked again and cached pages are
dropped if the file changed there.  Data goes through the buffer cache in
64 KiB blocks, pages are handed to the host directly (HGCM page lists), and
`VOP_GETPAGES` uses `vfs_bio_getpages(9)` so that mmap and
`read(2)`/`write(2)` share pages.  Writes are written behind the way the NFS
client does it: a complete 64 KiB block goes to the host at once, a partial
one stays dirty until a later write completes it, `close(2)`, `fsync(2)`,
`msync(2)`, or the syncer, which is asked to come by within
`vfs.vboxvfs.writedelay` seconds (default 1); mounting with `-o sync` makes
every `write(2)` reach the host before it returns.  Directory listings are fetched
whole and cached per open.  Rename, rmdir and remove keep the name cache in
step (`cache_vop_rename(9)`, `cache_vop_rmdir(9)`); unlinked files stay usable
while open.  Details and the reasons behind them are in the worklog (below).

## Building

Development build against a running kernel's sources, inside a guest that
has the VirtualBox port's work directory (the module links the guest ring-0
libraries `VBoxGuestR0Lib.a` and `RuntimeGuestR0.a` built by kmk):

    W=/usr/ports/emulators/virtualbox-ose-additions-72/work-xorg/VirtualBox-7.2.20
    sh scripts/dev/gen-opt-global.sh /root/kernopts     # opt_global.h of the running kernel
    make -C vboxvfs VBOX_SRC=$W KERNBUILDDIR=/root/kernopts
    make -C mount_vboxvfs
    kldload vboxvfs/vboxvfs.ko
    mount_vboxvfs/mount_vboxvfs -o uid=1001,gid=1001 share /mnt/share

`KERNBUILDDIR` matters on kernels with `INVARIANTS` or `BUF_TRACKING`: the
module must see the same `opt_global.h` as the kernel, or assertions and some
structure layouts differ.  The module itself never touches the layout-sensitive
members (it takes page addresses with `pmap_kextract(9)`), so the ports package,
which is built without kernel options, works on `GENERIC`.

The ports change is produced from this tree:

    sh ports/gen-ports-patch.sh /path/to/ports [/path/to/pristine/VirtualBox]

and builds with the VirtualBox kBuild tree (`vboxvfs/Makefile.kmk`,
`mount_vboxvfs/Makefile.kmk`).

## Testing

- `tests/smoke-browse.sh`, `smoke-modify.sh`, `smoke-io.sh`: quick checks run
  inside a guest against a mounted share.
- `tests/pjd/drive-pjd.sh VM OUT`: runs pjdfstest (with
  `tests/pjdfstest-vboxvfs.patch` applied with `git apply --directory=tests`)
  in the guest one file at a time, resumable across panics; the other scripts
  there turn the logs into the comparison tables used in the worklog.
- `tests/stress/run-stress.sh VM 600 OUT`: ten minutes of fsx, stress2, a
  copy/diff/git workload and concurrent changes from the host, with before and
  after checks; `tests/stress/coherence-check.sh VM` measures how long the
  guest takes to see files rewritten on the host.

Results so far (FreeBSD 15.1-RELEASE and 16-CURRENT with `INVARIANTS` and
`WITNESS`, VirtualBox 7.2.20 on a FreeBSD host): 171 of 238 pjdfstest files
pass completely, fsx with mmap passes, the stress run finds nothing.  The
remaining pjdfstest failures are limits of the protocol and of host side
policy, listed below.

## What the shared folder protocol cannot do

These are not bugs of the driver; the Linux `vboxsf` client has the same
limits.

- No device nodes, fifos, sockets or hard links (`EOPNOTSUPP`).
- Ownership is the mount's `uid`/`gid`; `chown(2)` to anything else fails
  with `EPERM` (the Linux client pretends to succeed).
- On Unix hosts the host keeps the owner read bit set and clears
  setuid/setgid/sticky when a mode is changed; `chmod 0222` reads back as
  `0722`.
- The guest's root is not root on the host: files without write permission
  cannot be written or truncated even by root in the guest.
- A symbolic link's own mode and times cannot be changed (the request is
  accepted and ignored); its target is never touched by `lchmod`/`lutimes`.
- `utimes(2)` with the current time uses the guest's clock, while files
  created on the host carry the host's; keep the guest clock synchronized
  (`vboxservice_flags="--timesync-set-start"` helps right after boot).
- Paths that are legal in the guest can exceed the host's limit once the
  share's path is prepended (`ENAMETOOLONG`).
- Changes made on the host become visible after the ttl; a read racing with
  a host side truncate can return the new bytes followed by zeros, as with any
  attribute cache (NFS behaves the same).
- Data written by the guest reaches the host within about a second (or at
  `close(2)`, whichever comes first); a guest panic or power-off loses what
  was not yet flushed, as with any write-behind cache.  Mount with `-o sync`
  where every `write(2)` must be on the host before it returns.

## Worklog

Design notes, environment setup, every test run and the reasoning behind the
decisions are kept in the companion repository
<https://github.com/lwhsu/freebsd-vboxfs-worklog>.

## Requirements

- FreeBSD 15.x or 16-CURRENT guest (amd64).  14.x would need the
  `getmntopts.c` fallback for `mount_vboxvfs`; not done.
- VirtualBox 7.2 sources (the FreeBSD port provides them).
- A VirtualBox host >= 6.1 for full functionality (page lists); tested with
  7.2.20.
