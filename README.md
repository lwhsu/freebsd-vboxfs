# freebsd-vboxfs

A rewrite of the VirtualBox shared folder client for FreeBSD guests:
the `vboxvfs` kernel module and the `mount_vboxvfs(8)` utility.

The code in this branch (`rewrite`) is new work, dual-licensed under
`GPL-3.0-only OR BSD-2-Clause` (see `LICENSE`).  It does not derive from the
2016 Solaris-based port that lives on the `master` branch and in the FreeBSD
ports tree (`emulators/virtualbox-ose-additions-72`); that code is kept only
for history.

## Layout

| Directory        | Contents |
|------------------|----------|
| `vboxvfs/`       | kernel module sources (`bsd.kmod.mk` and kBuild builds) |
| `mount_vboxvfs/` | `mount_vboxvfs(8)` and its manual page |
| `ports/`         | scripts that turn this tree into FreeBSD ports patches |
| `scripts/`       | development helpers: VM provisioning, sync to a VirtualBox WRKSRC, build |
| `tests/`         | pjdfstest local patch, stress test drivers |

## Status

Work in progress.  Design notes, environment setup and test results are kept
in the companion repository
<https://github.com/lwhsu/freebsd-vboxfs-worklog>.

## Requirements

- FreeBSD 15.x or 16-CURRENT guest (amd64).
- VirtualBox 7.2 sources (the module links against the guest ring-0
  libraries built by the VirtualBox tree; the FreeBSD port provides them).
- A VirtualBox host >= 6.1 for full functionality.
