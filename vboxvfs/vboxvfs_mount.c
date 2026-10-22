/*-
 * SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
 *
 * Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
 */

/*
 * vboxvfs -- VFS operations: module init, mount, unmount, root, statfs.
 */

#include "vboxvfs_internal.h"

#include <sys/limits.h>
#include <sys/module.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sx.h>

MALLOC_DEFINE(M_VBOXVFS, "vboxvfs", "VirtualBox shared folder structures");

SYSCTL_NODE(_vfs, OID_AUTO, vboxvfs, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "VirtualBox shared folder file system");

int vboxvfs_debug = 0;
SYSCTL_INT(_vfs_vboxvfs, OID_AUTO, debug, CTLFLAG_RWTUN, &vboxvfs_debug, 0,
    "Debug verbosity: 1 errors, 2 vnode operations, 3 host requests");

/*
 * Serialises changes to the parent/name of nodes (rename) against the
 * readers that walk up the node chain to build paths.
 */
struct sx vboxvfs_rename_lock;

static bool vboxvfs_host_ready;

/* File system specific mount options; the VFS layer knows the rest. */
static const char *vboxvfs_opts[] = {
	"from", "export",
	"uid", "gid", "file_mode", "dir_mode", "fmask", "dmask", "ttl",
	NULL
};

static int
vboxvfs_init(struct vfsconf *vfsp __unused)
{
	int error;

	sx_init(&vboxvfs_rename_lock, "vboxvfs rename");
	error = vboxvfs_host_init();
	if (error != 0) {
		printf("vboxvfs: cannot connect to the shared folder service "
		    "(error %d); is vboxguest loaded and the VM configured "
		    "with shared folders?\n", error);
		vboxvfs_host_ready = false;
	} else
		vboxvfs_host_ready = true;
	return (0);
}

static int
vboxvfs_uninit(struct vfsconf *vfsp __unused)
{

	if (vboxvfs_host_ready)
		vboxvfs_host_term();
	vboxvfs_host_ready = false;
	vboxvfs_host_destroylog();
	sx_destroy(&vboxvfs_rename_lock);
	return (0);
}

/*
 * Fetch a numeric mount option.  Absent options leave *valp alone.
 */
static int
vboxvfs_getnum(struct mount *mp, const char *name, int base, u_long max,
    u_long *valp)
{
	char *s, *ep;
	u_long v;
	int error;

	s = vfs_getopts(mp->mnt_optnew, name, &error);
	if (s == NULL) {
		if (error == ENOENT)
			return (0);
		vfs_mount_error(mp, "malformed %s option", name);
		return (error);
	}
	if (*s == '\0')
		goto bad;
	v = strtoul(s, &ep, base);
	if (*ep != '\0' || v > max)
		goto bad;
	*valp = v;
	return (0);
bad:
	vfs_mount_error(mp, "invalid %s option \"%s\"", name, s);
	return (EINVAL);
}

static int
vboxvfs_parseopts(struct mount *mp, struct vboxvfs_mount *vmp)
{
	u_long v;
	int error;

	v = vmp->vm_uid;
	if ((error = vboxvfs_getnum(mp, "uid", 10, UID_MAX, &v)) != 0)
		return (error);
	vmp->vm_uid = v;
	v = vmp->vm_gid;
	if ((error = vboxvfs_getnum(mp, "gid", 10, GID_MAX, &v)) != 0)
		return (error);
	vmp->vm_gid = v;
	v = vmp->vm_fmode;
	if ((error = vboxvfs_getnum(mp, "file_mode", 8, ALLPERMS, &v)) != 0)
		return (error);
	vmp->vm_fmode = v;
	v = vmp->vm_dmode;
	if ((error = vboxvfs_getnum(mp, "dir_mode", 8, ALLPERMS, &v)) != 0)
		return (error);
	vmp->vm_dmode = v;
	v = vmp->vm_fmask;
	if ((error = vboxvfs_getnum(mp, "fmask", 8, ALLPERMS, &v)) != 0)
		return (error);
	vmp->vm_fmask = v;
	v = vmp->vm_dmask;
	if ((error = vboxvfs_getnum(mp, "dmask", 8, ALLPERMS, &v)) != 0)
		return (error);
	vmp->vm_dmask = v;
	v = vmp->vm_ttl_ms;
	if ((error = vboxvfs_getnum(mp, "ttl", 10, INT_MAX, &v)) != 0)
		return (error);
	vmp->vm_ttl_ms = v;
	vmp->vm_ttl = (sbintime_t)vmp->vm_ttl_ms * SBT_1MS;
	return (0);
}

static int
vboxvfs_mount(struct mount *mp)
{
	struct vboxvfs_mount *vmp;
	struct vnode *vp;
	SHFLCREATEPARMS parms;
	SHFLVOLINFO vi;
	char *from;
	int error;

	if (mp->mnt_flag & MNT_ROOTFS)
		return (EOPNOTSUPP);
	if (vfs_filteropt(mp->mnt_optnew, vboxvfs_opts) != 0)
		return (EINVAL);

	if (mp->mnt_flag & MNT_UPDATE) {
		/*
		 * Only the generic flag changes (ro/rw and friends) are
		 * supported; they are applied by the VFS layer.  Refuse
		 * option changes and read-write remounts of read-only shares.
		 */
		vmp = VFSTOVBOXVFS(mp);
		if (vfs_getopt(mp->mnt_optnew, "export", NULL, NULL) == 0)
			return (EOPNOTSUPP);
		if (vfs_getopt_pos(mp->mnt_optnew, "uid") >= 0 ||
		    vfs_getopt_pos(mp->mnt_optnew, "gid") >= 0 ||
		    vfs_getopt_pos(mp->mnt_optnew, "file_mode") >= 0 ||
		    vfs_getopt_pos(mp->mnt_optnew, "dir_mode") >= 0 ||
		    vfs_getopt_pos(mp->mnt_optnew, "fmask") >= 0 ||
		    vfs_getopt_pos(mp->mnt_optnew, "dmask") >= 0 ||
		    vfs_getopt_pos(mp->mnt_optnew, "ttl") >= 0) {
			vfs_mount_error(mp,
			    "options cannot be changed with -u");
			return (EOPNOTSUPP);
		}
		if (vmp->vm_hostro && (mp->mnt_flag & MNT_RDONLY) == 0) {
			vfs_mount_error(mp, "the host exports the share "
			    "read-only");
			return (EROFS);
		}
		return (0);
	}

	if (!vboxvfs_host_ready) {
		vfs_mount_error(mp, "not connected to the host shared folder "
		    "service");
		return (ENXIO);
	}

	from = vfs_getopts(mp->mnt_optnew, "from", &error);
	if (from == NULL || from[0] == '\0') {
		vfs_mount_error(mp, "share name missing");
		return (EINVAL);
	}
	if (strlen(from) >= SHFL_MAX_LEN) {
		vfs_mount_error(mp, "share name too long");
		return (ENAMETOOLONG);
	}

	vmp = malloc(sizeof(*vmp), M_VBOXVFS, M_WAITOK | M_ZERO);
	vmp->vm_mp = mp;
	vmp->vm_root = SHFL_ROOT_NIL;
	vmp->vm_roothandle = SHFL_HANDLE_NIL;
	vmp->vm_ttl_ms = VBOXVFS_DEFAULT_TTL_MS;
	vmp->vm_ttl = (sbintime_t)VBOXVFS_DEFAULT_TTL_MS * SBT_1MS;
	vmp->vm_biosize = VBOXVFS_DEFAULT_BIOSIZE;
	vmp->vm_namemax = NAME_MAX;
	vmp->vm_share = strdup(from, M_VBOXVFS);
	error = vboxvfs_parseopts(mp, vmp);
	if (error != 0)
		goto fail;

	error = vboxvfs_host_map(from, &vmp->vm_root);
	if (error != 0) {
		vfs_mount_error(mp, "cannot map shared folder \"%s\": %s",
		    from, error == ENXIO ? "no such share" : "host error");
		goto fail;
	}

	/*
	 * Keep the root directory open for the lifetime of the mount; a
	 * handle is needed for volume information and this one also gives
	 * us the root's attributes.
	 */
	error = vboxvfs_host_create(vmp, "", 0, VBOXVFS_CREATE_DIR_RO, 0,
	    &parms);
	if (error == 0 && parms.Handle == SHFL_HANDLE_NIL)
		error = EIO;
	if (error != 0) {
		vfs_mount_error(mp, "cannot open the root of \"%s\"", from);
		goto fail;
	}
	vmp->vm_roothandle = parms.Handle;

	if (vboxvfs_host_volinfo(vmp, vmp->vm_roothandle, &vi) == 0) {
		vmp->vm_hostro = vi.fsProperties.fReadOnly;
		if (vi.fsProperties.cbMaxComponent != 0)
			vmp->vm_namemax = MIN(vi.fsProperties.cbMaxComponent,
			    NAME_MAX);
	}

	mp->mnt_data = vmp;
	MNT_ILOCK(mp);
	if (vmp->vm_hostro)
		mp->mnt_flag |= MNT_RDONLY;
	mp->mnt_kern_flag |= MNTK_NO_IOPF | MNTK_USES_BCACHE;
	MNT_IUNLOCK(mp);
	vfs_getnewfsid(mp);
	vfs_mountedfrom(mp, from);
	mp->mnt_stat.f_iosize = vmp->vm_biosize;

	error = vboxvfs_node_get(vmp, NULL, "", 0, &parms.Info, &vp);
	if (error != 0) {
		mp->mnt_data = NULL;
		goto fail;
	}
	vp->v_vflag |= VV_ROOT;
	vmp->vm_rootnode = VTOVBOXVFS(vp);
	/* The mount keeps this reference; vflush() in unmount drops it. */
	VOP_UNLOCK(vp);

	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "mounted share \"%s\" on %s "
	    "(root id %u%s)\n", from, mp->mnt_stat.f_mntonname, vmp->vm_root,
	    vmp->vm_hostro ? ", read-only" : "");
	return (0);

fail:
	if (vmp->vm_roothandle != SHFL_HANDLE_NIL)
		vboxvfs_host_close(vmp, vmp->vm_roothandle);
	if (vmp->vm_root != SHFL_ROOT_NIL)
		vboxvfs_host_unmap(vmp->vm_root);
	free(vmp->vm_share, M_VBOXVFS);
	free(vmp, M_VBOXVFS);
	return (error);
}

static int
vboxvfs_unmount(struct mount *mp, int mntflags)
{
	struct vboxvfs_mount *vmp;
	int error, flags, nvnodes;

	vmp = VFSTOVBOXVFS(mp);
	flags = 0;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	/*
	 * Nodes hold a reference on their parent directory, so one pass of
	 * vflush() only reclaims the leaves and reports the directories as
	 * busy.  Repeat while it keeps making progress.  The mount holds
	 * exactly one reference on the root, which vflush() releases.
	 */
	for (;;) {
		MNT_ILOCK(mp);
		nvnodes = mp->mnt_nvnodelistsize;
		MNT_IUNLOCK(mp);
		error = vflush(mp, 1, flags, curthread);
		if (error != EBUSY)
			break;
		MNT_ILOCK(mp);
		if (mp->mnt_nvnodelistsize >= nvnodes) {
			MNT_IUNLOCK(mp);
			break;
		}
		MNT_IUNLOCK(mp);
	}
	if (error != 0)
		return (error);
	KASSERT(vmp->vm_rootnode == NULL,
	    ("vboxvfs_unmount: root node survived vflush"));

	vboxvfs_host_close(vmp, vmp->vm_roothandle);
	vboxvfs_host_unmap(vmp->vm_root);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "unmounted share \"%s\"\n",
	    vmp->vm_share);
	free(vmp->vm_share, M_VBOXVFS);
	free(vmp, M_VBOXVFS);
	mp->mnt_data = NULL;
	return (0);
}

static int
vboxvfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	struct vboxvfs_mount *vmp;
	struct vnode *vp;
	int error;

	vmp = VFSTOVBOXVFS(mp);
	vp = VBOXVFSTOV(vmp->vm_rootnode);
	error = vget(vp, flags);
	if (error != 0)
		return (error);
	*vpp = vp;
	return (0);
}

static int
vboxvfs_statfs(struct mount *mp, struct statfs *sbp)
{
	struct vboxvfs_mount *vmp;
	SHFLVOLINFO vi;
	uint64_t bsize;
	int error;

	vmp = VFSTOVBOXVFS(mp);
	error = vboxvfs_host_volinfo(vmp, vmp->vm_roothandle, &vi);
	if (error != 0)
		return (error);
	bsize = vi.ulBytesPerAllocationUnit != 0 ?
	    vi.ulBytesPerAllocationUnit : PAGE_SIZE;
	sbp->f_bsize = bsize;
	sbp->f_iosize = vmp->vm_biosize;
	sbp->f_blocks = vi.ullTotalAllocationBytes / bsize;
	sbp->f_bfree = vi.ullAvailableAllocationBytes / bsize;
	sbp->f_bavail = sbp->f_bfree;
	/* The host does not count inodes; report plenty. */
	sbp->f_files = UINT32_MAX;
	sbp->f_ffree = UINT32_MAX;
	sbp->f_namemax = vmp->vm_namemax;
	return (0);
}

static struct vfsops vboxvfs_vfsops = {
	.vfs_init =	vboxvfs_init,
	.vfs_uninit =	vboxvfs_uninit,
	.vfs_mount =	vboxvfs_mount,
	.vfs_unmount =	vboxvfs_unmount,
	.vfs_root =	vboxvfs_root,
	.vfs_statfs =	vboxvfs_statfs,
};

VFS_SET(vboxvfs_vfsops, vboxvfs, VFCF_NETWORK);
MODULE_DEPEND(vboxvfs, vboxguest, 1, 1, 1);
