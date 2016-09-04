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
	.vfs_unmount	= vboxfs_unmount
};


VFS_SET(vboxfs_vfsops, vboxvfs, VFCF_NETWORK);
MODULE_DEPEND(vboxvfs, vboxguest, 1, 1, 1);

/*
 * Allocates a new node of type 'type' inside the 'tmp' mount point, with
 * its owner set to 'uid', its group to 'gid' and its mode set to 'mode',
 * using the credentials of the process 'p'.
 *
 * If the node type is set to 'VDIR', then the parent parameter must point
 * to the parent directory of the node being created.  It may only be NULL
 * while allocating the root node.
 *
 * If the node type is set to 'VBLK' or 'VCHR', then the rdev parameter
 * specifies the device the node represents.
 *
 * If the node type is set to 'VLNK', then the parameter target specifies
 * the file name of the target file for the symbolic link that is being
 * created.
 *
 * Note that new nodes are retrieved from the available list if it has
 * items or, if it is empty, from the node pool as long as there is enough
 * space to create them.
 *
 * Returns zero on success or an appropriate error code on failure.
 */
int
vboxfs_alloc_node(struct mount *mp, struct vboxfs_mnt *vsfmp, const char *fullpath,
    enum vtype type, uid_t uid, gid_t gid, mode_t mode, struct vboxfs_node *parent,
    struct vboxfs_node **node)
{
	struct vboxfs_node *nnode;

	if ((mp->mnt_kern_flag & MNTK_UNMOUNT) != 0) {
		/*
		 * When a new tmpfs node is created for fully
		 * constructed mount point, there must be a parent
		 * node, which vnode is locked exclusively.  As
		 * consequence, if the unmount is executing in
		 * parallel, vflush() cannot reclaim the parent vnode.
		 * Due to this, the check for MNTK_UNMOUNT flag is not
		 * racy: if we did not see MNTK_UNMOUNT flag, then tmp
		 * cannot be destroyed until node construction is
		 * finished and the parent vnode unlocked.
		 *
		 * Tmpfs does not need to instantiate new nodes during
		 * unmount.
		 */
		return (EBUSY);
	}

	nnode = (struct vboxfs_node *)uma_zalloc_arg(
				vsfmp->sf_node_pool, vsfmp, M_WAITOK);

	/* Generic initialization. */
	nnode->sf_type = type;
	nnode->sf_ino = vsfmp->sf_ino++;
	nnode->sf_path = strdup(fullpath, M_VBOXVFS);
	nnode->sf_parent = parent;
	nnode->vboxfsmp = vsfmp;

	/* Type-specific initialization. */
	switch (nnode->sf_type) {
	case VBLK:
	case VCHR:
	case VDIR:
	case VFIFO:
	case VSOCK:
	case VLNK:
	case VREG:
		break;

	default:
		panic("vboxfs_alloc_node: type %p %d", nnode, (int)nnode->sf_type);
	}

	*node = nnode;
	return 0;
}

void
vboxfs_free_node(struct vboxfs_mnt *vboxfs, struct vboxfs_node *node)
{

#ifdef INVARIANTS
	TMPFS_NODE_LOCK(node);
	MPASS(node->sf_vnode == NULL);
	MPASS((node->sf_vpstate & TMPFS_VNODE_ALLOCATING) == 0);
	TMPFS_NODE_UNLOCK(node);
#endif
	if (node->sf_path)
		free(node->sf_path, M_VBOXVFS);

	uma_zfree(vboxfs->sf_node_pool, node);
}

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
vboxfs_node_ctor(void *mem, int size, void *arg, int flags)
{
	struct vboxfs_node *node = (struct vboxfs_node *)mem;

	node->sf_vnode = NULL;
	node->sf_vpstate = 0;

	return (0);
}

static void
vboxfs_node_dtor(void *mem, int size, void *arg)
{
	struct vboxfs_node *node = (struct vboxfs_node *)mem;
	node->sf_type = VNON;
}

static int
vboxfs_node_init(void *mem, int size, int flags)
{
	struct vboxfs_node *node = (struct vboxfs_node *)mem;
	node->sf_ino = 0;

	mtx_init(&node->sf_interlock, "tmpfs node interlock", NULL, MTX_DEF);

	return (0);
}

static void
vboxfs_node_fini(void *mem, int size)
{
	struct vboxfs_node *node = (struct vboxfs_node *)mem;

	mtx_destroy(&node->sf_interlock);
}

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
	struct vboxfs_node *root;

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
	vboxfsmp->sf_stat_ttl = 200;

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

	vboxfsmp->sf_node_pool = uma_zcreate("VBOXFS node",
	    sizeof(struct vboxfs_node),
	    vboxfs_node_ctor, vboxfs_node_dtor,
	    vboxfs_node_init, vboxfs_node_fini,
	    UMA_ALIGN_PTR, 0);

	/* Allocate the root node. */
	error = vboxfs_alloc_node(mp, vboxfsmp, "", VDIR, 0,
	    0, 0755, NULL, &root);

	if (error != 0 || root == NULL) {
		uma_zdestroy(vboxfsmp->sf_node_pool);
		free(vboxfsmp, M_VBOXVFS);
		return error;
	}

	root->sf_parent = root;
	vboxfsmp->sf_root = root;

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

	uma_zdestroy(vboxfsmp->sf_node_pool);

	free(vboxfsmp, M_VBOXVFS);
	MNT_ILOCK(mp);
	mp->mnt_data = NULL;
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);

	return (0);
}

static int
vboxfs_root(struct mount *mp, int flags, struct vnode **vpp)
{
        int error;
        error = vboxfs_alloc_vp(mp, VFSTOVBOXFS(mp)->sf_root, flags, vpp);

        if (!error)
                (*vpp)->v_vflag |= VV_ROOT;

        return error;
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

	error = sfprov_get_fsinfo(vboxfsmp->sf_handle, &fsinfo);
	if (error != 0)
		return (error);

	sbp->f_iosize = fsinfo.blksize;
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
