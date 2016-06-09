/*
 * Copyright (C) 2008-2010 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/dirent.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/fcntl.h>
#include <sys/priv.h>
#include <sys/stat.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/sbuf.h>

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include "vboxvfs.h"

#define VFSMP2SFGLOBINFO(mp) ((struct sf_glob_info *)mp->mnt_data)

#ifdef MALLOC_DECLARE
MALLOC_DEFINE(M_VBOXVFS, "vboxvfs", "VBOX VFS");
#endif

static sfp_connection_t *sfprov = NULL;

static int vboxfs_version = VBOXVFS_VERSION;
u_int vboxvfs_debug = 1;

SYSCTL_NODE(_vfs, OID_AUTO, vboxfs, CTLFLAG_RW, 0, "VirtualBox shared filesystem");
SYSCTL_INT(_vfs_vboxfs, OID_AUTO, version, CTLFLAG_RD, &vboxfs_version, 0, "");
SYSCTL_UINT(_vfs_vboxfs, OID_AUTO, debug, CTLFLAG_RW, &vboxvfs_debug, 0, "Debug level");

/* global connection to the host service. */
//static VBSFCLIENT g_vboxSFClient;

static vfs_init_t	vboxfs_init;
static vfs_uninit_t	vboxfs_uninit;
static vfs_cmount_t	vboxfs_cmount;
static vfs_mount_t	vboxfs_mount;
static vfs_root_t	vboxfs_root;
static vfs_quotactl_t	vboxfs_quotactl;
static vfs_statfs_t	vboxfs_statfs;
static vfs_unmount_t	vboxfs_unmount;

static struct vfsops vboxfs_vfsops = {
	.vfs_init	= vboxfs_init,
	.vfs_cmount	= vboxfs_cmount,
	.vfs_mount	= vboxfs_mount,
	.vfs_quotactl	= vboxfs_quotactl,
	.vfs_root	= vboxfs_root,
	.vfs_statfs	= vboxfs_statfs,
	.vfs_sync	= vfs_stdsync,
	.vfs_uninit	= vboxfs_uninit,
	.vfs_unmount	= vboxfs_unmount,
	.vfs_vget	= vboxfs_vget,
};


VFS_SET(vboxfs_vfsops, vboxvfs, VFCF_NETWORK);
MODULE_DEPEND(vboxvfs, vboxguest, 1, 1, 1);

static int
vboxfs_cmount(struct mntarg *ma, void *data, uint64_t flags)
{
	struct vboxfs_mount_info args;
	int error = 0;

	if (data == NULL)
		return (EINVAL);
	error = copyin(data, &args, sizeof(struct vboxfs_mount_info));
	if (error)
		return (error);

  	ma = mount_argf(ma, "uid", "%d", args.uid);
	ma = mount_argf(ma, "gid", "%d", args.gid);
	ma = mount_argf(ma, "file_mode", "%d", args.fmode);
	ma = mount_argf(ma, "dir_mode", "%d", args.dmode);
	ma = mount_arg(ma, "from", args.name, -1);
	return (kernel_mount(ma, flags));
}

static const char *vboxfs_opts[] = {
	"fstype",
	"fspath",
	"from",
	"uid",
	"gid",
	"file_mode",
	"dir_mode",
	"errmsg",
	NULL
};

#define	VBOX_INTOPT(optname, val, base) do {				\
	char *ep, *optarg = NULL;					\
	if (vfs_getopt(opts, optname, (void **)&optarg, NULL) == 0) {	\
		if (optarg != NULL && *optarg == '\0')			\
			optarg = NULL;					\
		if (optarg != NULL)					\
			val = strtoul(optarg, &ep, base);		\
		if (optarg == NULL || *ep != '\0') {			\
			struct sbuf *sb = sbuf_new_auto();		\
			sbuf_printf(sb, "Invalid %s: \"%s\"", optname,	\
			    optarg);					\
			sbuf_finish(sb);				\
			vfs_mount_error(mp, sbuf_data(sb));		\
			sbuf_delete(sb);				\
			return (EINVAL);				\
		}							\
	}								\
} while (0)

static int
vboxfs_mount(struct mount *mp)
{
	struct vboxfs_mnt *vboxfsmp = NULL; 
	struct vfsoptlist *opts = mp->mnt_optnew;
	sfp_mount_t *handle = NULL;
	int readonly = 0;
	sffs_fsinfo_t fsinfo;
	int error, share_len;
	char *share_name;
   	mode_t file_mode = 0, dir_mode = 0;
	uid_t uid = 0;
	gid_t gid = 0;

	if (mp->mnt_flag & (MNT_UPDATE | MNT_ROOTFS))
		return (EOPNOTSUPP);

	if (vfs_filteropt(opts, vboxfs_opts)) {
		vfs_mount_error(mp, "%s", "Invalid option");
		return (EINVAL);
	}

	VBOX_INTOPT("uid", uid, 10);
	VBOX_INTOPT("gid", gid, 10);
	VBOX_INTOPT("file_mode", file_mode, 8);
	VBOX_INTOPT("dir_mode", dir_mode, 8);
	VBOX_INTOPT("ro", readonly, 10);

	error = vfs_getopt(opts, "from", (void **)&share_name, &share_len);
	if (error != 0 || share_len == 0) {
		vfs_mount_error(mp, "Invalid from");
		return (EINVAL);
	}

	vboxfsmp = malloc(sizeof(struct vboxfs_mnt), M_VBOXVFS, M_WAITOK | M_ZERO);
	vboxfsmp->sf_uid = uid;
	vboxfsmp->sf_gid = gid;
	vboxfsmp->sf_fmode = file_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
	vboxfsmp->sf_dmode = dir_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
	vboxfsmp->sf_ino = 3;

	/* Invoke Hypervisor mount interface before proceeding */
	error = sfprov_mount(share_name, &handle);
	if (error)
		return (error);

	/* Determine whether the filesystem must be read-only. */
	error = sfprov_get_fsinfo(handle, &fsinfo);
	if (error != 0) {
		sfprov_unmount(handle);
		return (error);
	}
	if (readonly == 0)
		readonly = (fsinfo.readonly != 0);

	vboxfsmp->sf_handle = handle;
	vboxfsmp->sf_vfsp = mp;

	MNT_ILOCK(mp);
	mp->mnt_data = vboxfsmp;
	bzero(&mp->mnt_stat.f_fsid, sizeof(&mp->mnt_stat.f_fsid));
	/* f_fsid is int32_t but serial is uint32_t, convert */
	memcpy(&mp->mnt_stat.f_fsid, &fsinfo.serial, sizeof(mp->mnt_stat.f_fsid));
	mp->mnt_flag |= MNT_LOCAL;
	if (readonly != 0)
		mp->mnt_flag |= MNT_RDONLY;
#if __FreeBSD_version >= 1000021
	mp->mnt_kern_flag |= MNTK_LOOKUP_SHARED | MNTK_EXTENDED_SHARED;
#else
	mp->mnt_kern_flag |= MNTK_MPSAFE | MNTK_LOOKUP_SHARED |
	    MNTK_EXTENDED_SHARED;
#endif
	MNT_IUNLOCK(mp);
	vfs_mountedfrom(mp, share_name);

	return (0);
}

/*
 * Unmount a shared folder.
 *
 * vboxfs_unmount umounts the mounted file system. It return 0 
 * on sucess and any relevant errno on failure.
 */
static int
vboxfs_unmount(struct mount *mp, int mntflags)
{
	struct vboxfs_mnt *vboxfsmp; 
	struct thread *td;
	int error;
	int flags;

	printf("vboxfs_unmount: flags=%04x\n", mntflags);		
	vboxfsmp = VFSTOVBOXFS(mp);
	td = curthread;
	flags = 0;
	if (mntflags & MNT_FORCE)
		flags |= FORCECLOSE;

	error = vflush(mp, 0, flags, td);
	if (error)
		return (error);

	/* Invoke Hypervisor unmount interface before proceeding */
	error = sfprov_unmount(vboxfsmp->sf_handle);
	if (error != 0) {
		/* TBD anything here? */
	}

	free(vboxfsmp, M_VBOXVFS);
	MNT_ILOCK(mp);
	mp->mnt_data = NULL;
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);

	printf("vboxfs_unmount() done\n");

	return (0);
}

static int
vboxfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
	int error;
	struct vboxfs_node *unode;

	error = (vboxfs_vget(mp, (ino_t)ROOTDIR_INO, flags, vpp));
	if (!error) {
		unode = (*vpp)->v_data;
		unode->sf_path = strdup("/", M_VBOXVFS);
	}
	return (error);
}

int
vboxfs_vget(struct mount *mp, ino_t ino, int flags, struct vnode **vpp)
{
	struct vboxfs_mnt *vboxfsmp;
	struct thread *td;
	struct vnode *vp;
	struct vboxfs_node *unode;
	int error;

	error = vfs_hash_get(mp, ino, flags, curthread, vpp, NULL, NULL);
	if (error || *vpp != NULL)
		return (error);

	/*
	 * We must promote to an exclusive lock for vnode creation.  This
	 * can happen if lookup is passed LOCKSHARED.
 	 */
	if ((flags & LK_TYPE_MASK) == LK_SHARED) {
		flags &= ~LK_TYPE_MASK;
		flags |= LK_EXCLUSIVE;
	}

	/*
	 * We do not lock vnode creation as it is believed to be too
	 * expensive for such rare case as simultaneous creation of vnode
	 * for same ino by different processes. We just allow them to race
	 * and check later to decide who wins. Let the race begin!
	 */
	td = curthread;
	if ((error = vboxfs_allocv(mp, &vp, td))) {
		printf("Error from vboxfs_allocv\n");
		return (error);
	}

	vboxfsmp = VFSTOVBOXFS(mp);
	unode = malloc(sizeof(struct vboxfs_node), M_VBOXVFS, M_WAITOK | M_ZERO);
	unode->sf_vnode = vp;
	unode->sf_ino = ino;
	unode->vboxfsmp = vboxfsmp;
	vp->v_data = unode;

	lockmgr(vp->v_vnlock, LK_EXCLUSIVE, NULL);
	error = insmntque(vp, mp);
	if (error != 0)
		return (error);

	error = vfs_hash_insert(vp, ino, flags, td, vpp, NULL, NULL);
	if (error || *vpp != NULL)
		return (error);

	switch (ino) {
	case ROOTDIR_INO:
		vp->v_type = VDIR;
		break;
	case THEFILE_INO:
		vp->v_type = VREG;
		break;
	default:
		vp->v_type = VBAD;
		break;
	}

	if (vp->v_type != VFIFO)
		VN_LOCK_ASHARE(vp);

	if (ino == ROOTDIR_INO)
		vp->v_vflag |= VV_ROOT;

	*vpp = vp;

	return (0);
}

/*
 * Do operation associated with quotas, not supported
 */
static int
vboxfs_quotactl(struct mount *mp, int cmd, uid_t uid, void *arg)
{
	return (EOPNOTSUPP);
}

/*
 * Initialize the filesystem globals.
 */
static int
vboxfs_init(struct vfsconf *vfsp)
{
	int error;

	DROP_GIANT();

	sfprov = sfprov_connect(SFPROV_VERSION);
	if (sfprov == NULL) {
		printf("%s: couldn't connect to sf provider", __func__);
		return (ENODEV);
	}

	error = sfprov_set_show_symlinks();
	if (error != 0)
		printf("%s: host unable to show symlinks, error=%d\n",
		    __func__, error);

	PICKUP_GIANT();
	return (0);
}

/*
 * Undo the work of vboxfs_init().
 */
static int
vboxfs_uninit(struct vfsconf *vfsp)
{

	DROP_GIANT();
	sfprov_disconnect();
	PICKUP_GIANT();
	return (0);
}

/*
 * Get filesystem statistics.
 */
static int
vboxfs_statfs(struct mount *mp, struct statfs *sbp)
{
	struct vboxfs_mnt *vboxfsmp;
	sffs_fsinfo_t fsinfo;
	int error;

	vboxfsmp = VFSTOVBOXFS(mp);

	bzero(sbp, sizeof(*sbp));
	error = sfprov_get_fsinfo(vboxfsmp->sf_handle, &fsinfo);
	if (error != 0)
		return (error);

	sbp->f_bsize = fsinfo.blksize;

	sbp->f_bfree = fsinfo.blksavail;
	sbp->f_bavail = fsinfo.blksavail;
	sbp->f_files = fsinfo.blksavail / 4; /* some kind of reasonable value */
	sbp->f_ffree = fsinfo.blksavail / 4;

	sbp->f_blocks = fsinfo.blksused + sbp->f_bavail;
	sbp->f_fsid.val[0] = mp->mnt_stat.f_fsid.val[0];
	sbp->f_fsid.val[1] = mp->mnt_stat.f_fsid.val[1];
	sbp->f_namemax = fsinfo.maxnamesize;

	return (0);
}
