/* $Id: vboxfs_vfsops.c $ */
/** @file
 * Description.
 */

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

#include <geom/geom.h>
#include <geom/geom_vfs.h>

#include "vboxvfs.h"

#define VFSMP2SFGLOBINFO(mp) ((struct sf_glob_info *)mp->mnt_data)

#ifdef MALLOC_DECLARE
MALLOC_DEFINE(M_VBOXVFS, "vboxvfs", "VBOX VFS");
#endif

static int vboxfs_version = VBOXVFS_VERSION;
static u_int vboxvfs_debug = 0;

SYSCTL_NODE(_vfs, OID_AUTO, vboxfs, CTLFLAG_RW, 0, "VirtualBox shared filesystem");
SYSCTL_INT(_vfs_vboxfs, OID_AUTO, version, CTLFLAG_RD, &vboxfs_version, 0, "");
SYSCTL_UINT(_vfs_vboxfs, OID_AUTO, debug, CTLFLAG_RW, &vboxvfs_debug, 0, "Debug level");

/* global connection to the host service. */
static VBSFCLIENT g_vboxSFClient;
static sfp_connection_t *sfprov = NULL;

static vfs_init_t       vboxfs_init;
static vfs_uninit_t     vboxfs_uninit;
static vfs_cmount_t     vboxfs_cmount;
static vfs_mount_t      vboxfs_mount;
static vfs_root_t       vboxfs_root;
static vfs_quotactl_t   vboxfs_quotactl;
static vfs_statfs_t     vboxfs_statfs;
static vfs_unmount_t    vboxfs_unmount;

static struct vfsops vboxfs_vfsops = {
    	.vfs_init     =		vboxfs_init,
    	.vfs_cmount   =    	vboxfs_cmount,
    	.vfs_mount    =    	vboxfs_mount,
    	.vfs_quotactl =    	vboxfs_quotactl,
    	.vfs_root     =    	vboxfs_root,
    	.vfs_statfs   =    	vboxfs_statfs,
    	.vfs_sync     =    	vfs_stdsync,
    	.vfs_uninit   =		vboxfs_uninit,
    	.vfs_unmount  =   	vboxfs_unmount,
	.vfs_vget     =		vboxfs_vget,
};


VFS_SET(vboxfs_vfsops, vboxvfs, VFCF_NETWORK);
MODULE_DEPEND(vboxvfs, vboxguest, 1, 1, 1);

static int vboxfs_cmount(struct mntarg *ma, void *data, uint64_t flags)
{
    	struct vboxfs_mount_info args;
    	int error = 0;

	VBOXVFS_DEBUG(1, "%s: Enter", __FUNCTION__);
	
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

    	error = kernel_mount(ma, flags);

	VBOXVFS_DEBUG(1, "%s: Leave error=%d", __FUNCTION__, error);

    	return (error);
};

static const char *vboxfs_opts[] = {
    	"fstype", "fspath", "from", "uid", "gid", "file_mode", "dir_mode", "errmsg", NULL
};

static int vboxfs_mount(struct mount *mp)
{
	struct vboxfs_mnt *vboxfsmp = NULL; 
	struct vboxfs_node *unode; 
	struct buf *bp = NULL;
	struct cdev *dev;
	struct g_consumer *cp;
	struct bufobj *bo;
	struct vnode *devvp;	/* vnode of the mount device */
	struct thread *td = curthread;
	struct vfsoptlist *opts = mp->mnt_optnew;
	struct nameidata nd, *ndp = &nd;
	sfp_mount_t *handle;
    	int error, share_len;
    	char *share_name;
   	mode_t file_mode = 0, dir_mode = 0;
	char *tmp, *ep;
	uid_t uid = 0;
	gid_t gid = 0;

	VBOXVFS_DEBUG(1, "%s: Enter", __FUNCTION__);

    	if (mp->mnt_flag & (MNT_UPDATE | MNT_ROOTFS))
        	return (EOPNOTSUPP);

    	if (vfs_filteropt(opts, vboxfs_opts))
    	{
        	vfs_mount_error(mp, "%s", "Invalid option");
        	return (EINVAL);
    	}

	if (vfs_getopt(opts, "uid", (void **)&tmp, NULL) == 0) {
        	if (tmp != NULL)
        		uid = (uid_t)strtol(tmp, &ep, 10);
       		if (tmp == NULL || *ep) {
       			vfs_mount_error(mp, "Invalid uid");
       			return (EINVAL);
       		}
        }
		
	if (vfs_getopt(opts, "gid", (void **)&tmp, NULL) == 0) {
        	if (tmp != NULL)
        		gid = (gid_t)strtol(tmp, &ep, 10);
       		if (tmp == NULL || *ep) {
       			vfs_mount_error(mp, "Invalid gid");
       			return (EINVAL);
       		}
        }

	if (vfs_getopt(opts, "file_mode", (void **)&tmp, NULL) == 0) {
        	if (tmp != NULL)
        		file_mode = (mode_t)strtol(tmp, &ep, 8);
#if 0
      		if (tmp == NULL || *ep) {
     			vfs_mount_error(mp, "Invalid file_mode");
    			return (EINVAL);
		}
#endif
		file_mode &= S_IRWXU | S_IRWXG | S_IRWXO;
        }

	if (vfs_getopt(opts, "dir_mode", (void **)&tmp, NULL) == 0) {
        	if (tmp != NULL)
        		dir_mode = (mode_t)strtol(tmp, &ep, 8);
#if 0
       		if (tmp == NULL || *ep) {
      			vfs_mount_error(mp, "Invalid dir_mode");
       			return (EINVAL);
       		}
#endif
		dir_mode &= S_IRWXU | S_IRWXG | S_IRWXO;
        }

	vboxfsmp = malloc(sizeof(struct vboxfs_mnt), M_VBOXVFS, M_WAITOK | M_ZERO);
        vboxfsmp->sf_uid = uid;
        vboxfsmp->sf_gid = gid;
        vboxfsmp->sf_fmode = file_mode;
        vboxfsmp->sf_dmode = dir_mode;

	/*
         * Invoke Hypervisor mount interface before proceeding
         */
	error = sfprov_mount(share_name, &handle);
	VBOXVFS_DEBUG(1, "sfprov_mount: error = %d", error);
	if (error) {
		return (error);
	}

    	mp->mnt_data = handle;

    	error = vfs_getopt(opts, "from", (void **)&share_name, &share_len);

    	if (error || share_name[share_len - 1] != '\0' || share_len > 0xfffe)
    	{
        	vfs_mount_error(mp, "Invalid from");
        	return (EINVAL);
	}

	/* Check that the mount device exists */
	if (share_name == NULL)
		return (EINVAL);

	VBOXVFS_DEBUG(1, "share_name: [%s]", share_name);

	NDINIT(ndp, LOOKUP, FOLLOW | LOCKLEAF, UIO_SYSSPACE, share_name, td);
	VBOXVFS_DEBUG(1, "Device exist_0 %d", namei(ndp));
	if ((error = namei(ndp)))
		return (error);
	VBOXVFS_DEBUG(1, "Device exist_1");
	NDFREE(ndp, NDF_ONLY_PNBUF);
	devvp = ndp->ni_vp;

	VBOXVFS_DEBUG(1, "Device exist_2");
	if (vn_isdisk(devvp, &error) == 0) {
		vput(devvp);
		return (error);
	}

	VBOXVFS_DEBUG(1, "Device exist_3");
	/* Check the access rights on the mount device */
	error = VOP_ACCESS(devvp, VREAD, td->td_ucred, td);
	if (error)
		error = priv_check(td, PRIV_VFS_MOUNT_PERM);
	if (error) {
		vput(devvp);
		return (error);
	}

	VBOXVFS_DEBUG(1, "Device exist_4");
	dev = devvp->v_rdev;
	dev_ref(dev);
	DROP_GIANT();
	g_topology_lock();
	error = g_vfs_open(devvp, &cp, "vboxvfs", 0);
	g_topology_unlock();
	PICKUP_GIANT();
	VOP_UNLOCK(devvp, 0);
	if (error)
		goto bail;

	bo = &devvp->v_bufobj;

	if (devvp->v_rdev->si_iosize_max != 0)
		mp->mnt_iosize_max = devvp->v_rdev->si_iosize_max;
	if (mp->mnt_iosize_max > MAXPHYS)
		mp->mnt_iosize_max = MAXPHYS;

	mp->mnt_data = vboxfsmp;
	mp->mnt_stat.f_fsid.val[0] = dev2udev(devvp->v_rdev);
	mp->mnt_stat.f_fsid.val[1] = mp->mnt_vfc->vfc_typenum;
	MNT_ILOCK(mp);
	mp->mnt_flag |= MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_MPSAFE | MNTK_LOOKUP_SHARED |
	    MNTK_EXTENDED_SHARED;
	MNT_IUNLOCK(mp);
	vboxfsmp->sf_vfsp = mp;
	vboxfsmp->sf_dev = dev;
	vboxfsmp->sf_devvp = devvp;
	vboxfsmp->sf_cp = cp;
	vboxfsmp->sf_bo = bo;
	vboxfsmp->size = cp->provider->mediasize;
	vboxfsmp->bsize = cp->provider->sectorsize;
	vboxfsmp->bmask = vboxfsmp->bsize - 1;
	vboxfsmp->bshift = ffs(vboxfsmp->bsize) - 1;

    	vfs_getnewfsid(mp);
    	vfs_mountedfrom(mp, share_name);

	VBOXVFS_DEBUG(1, "%s: Leave error=0", __FUNCTION__);

    	return (0);
bail:
	if (vboxfsmp != NULL)
		free(vboxfsmp, M_VBOXVFS);
	if (bp != NULL)
		brelse(bp);
	if (cp != NULL) {
		DROP_GIANT();
		g_topology_lock();
		g_vfs_close(cp);
		g_topology_unlock();
		PICKUP_GIANT();
	}
	dev_rel(dev);
	vrele(devvp);
	VBOXVFS_DEBUG(1, "%s: Leave error=%d", __FUNCTION__, error);
	return error;
};

/*
 * Unmount a shared folder.
 *
 * vboxfs_unmount umounts the mounted file system. It return 0 
 * on sucess and any relevant errno on failure.
 */
static int vboxfs_unmount(struct mount *mp, int mntflags)
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

    	/* There is 1 extra root vnode reference (vnode_root). */
    	error = vflush(mp, 1, flags, td);
    	if (error)
        	return (error);

	/*
         * Invoke Hypervisor unmount interface before proceeding
         */
        error = sfprov_unmount(vboxfsmp->sf_handle);
        if (error != 0) {
                /* TBD anything here? */
        }

        free(vboxfsmp->sf_share_name, M_VBOXVFS);
        free(vboxfsmp->sf_mntpath, M_VBOXVFS);
        free(vboxfsmp, M_VBOXVFS);
	mp->mnt_data = NULL;
	MNT_ILOCK(mp);
	mp->mnt_flag &= ~MNT_LOCAL;
	MNT_IUNLOCK(mp);

        printf("vboxfs_unmount() done\n");

    	return (0);
}

static int vboxfs_root(struct mount *mp, int flags, struct vnode **vpp)
{

	VBOXVFS_DEBUG(1, "%s: Enter", __FUNCTION__);

	ino_t id = 1;

	VBOXVFS_DEBUG(1, "%s: Leave", __FUNCTION__);

	return (vboxfs_vget(mp, id, flags, vpp));	
}

int
vboxfs_vget(struct mount *mp, ino_t ino, int flags, struct vnode **vpp)
{
	struct vboxfs_mnt *vboxfsmp;
	struct thread *td;
	struct vnode *vp;
	struct vboxfs_node *unode;
	int error;

	VBOXVFS_DEBUG(1, "%s: Enter", __FUNCTION__);

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
	default:
		vp->v_type = VBAD;
		break;
	case ROOTDIR_INO:
		vp->v_type = VDIR;
		break;
	case THEFILE_INO:
		vp->v_type = VREG;
		break;
	}

	if (vp->v_type != VFIFO)
		VN_LOCK_ASHARE(vp);

	if (ino == ROOTDIR_INO)
		vp->v_vflag |= VV_ROOT;

	*vpp = vp;

	VBOXVFS_DEBUG(1, "%s: Leave", __FUNCTION__);

	return (0);
}

/*
 * Do operation associated with quotas, not supported
 */
static int vboxfs_quotactl(struct mount *mp, int cmd, uid_t uid, void *arg)
{
	VBOXVFS_DEBUG(1, "%s", __FUNCTION__);
    	return (EOPNOTSUPP);
}

/*
 * Initialize the filesystem.
 */
static int vboxfs_init(struct vfsconf *vfsp)
{
	int rc = -1;

	VBOXVFS_DEBUG(1, "%s: Enter", __FUNCTION__);

	/* Initialize the R0 guest library. */
    	rc = vboxInit();
    	if (RT_FAILURE(rc))
	{
		printf("vboxInit failed rc=%d\n", rc);
        	return (EPROTO);
	}

    	/* Connect to the host service. */
    	rc = vboxConnect(&g_vboxSFClient);
    	if (RT_FAILURE(rc))
    	{
        	printf("Failed to get connection to host! rc=%d\n", rc);
        	vboxUninit();
        	return (EPROTO);
    	} 

    	rc = vboxCallSetUtf8(&g_vboxSFClient);
    	if (RT_FAILURE (rc))
    	{
        	printf("vboxCallSetUtf8 failed, rc=%d\n", rc);
        	vboxDisconnect(&g_vboxSFClient);
        	vboxUninit();
       		return (EPROTO);
    	}

    	printf("Successfully loaded shared folder module %d\n", vboxfs_version);

	int error;

        printf("vboxfs_init()\n");

        /*
         * This may seem a silly way to do things for now. But the code
         * is structured to easily allow it to be used on other hypervisors
         * which would have a different implementation of the provider.
         * Hopefully that'll never happen. :)
         */
        sfprov = sfprov_connect(SFPROV_VERSION);
        if (sfprov == NULL) {
                printf("vbox_init: couldn't init sffs provider");
                return (ENODEV);
        }

        error = sfprov_set_show_symlinks();
        if (error != 0) {
                printf("sffs_init: host unable to show symlinks, "
                                                  "error=%d\n", error);
        }

	VBOXVFS_DEBUG(1, "%s: Leave", __FUNCTION__);
    	return (0);
}

/*
 * Undo the work of vboxfs_init().
 */
static int vboxfs_uninit(struct vfsconf *vfsp)
{

	VBOXVFS_DEBUG(1, "%s: Enter", __FUNCTION__);

    	vboxDisconnect(&g_vboxSFClient);
    	vboxUninit();
	/*
	 * close connection to the provider
	 */
	sfprov_disconnect(sfprov);

	VBOXVFS_DEBUG(1, "%s: Leave", __FUNCTION__);

    	return (0);
}

/*
 * Get filesystem statistics.
 */
static int vboxfs_statfs(struct mount *mp, struct statfs *sbp)
{
	struct vboxfs_mnt *vboxfsmp;
        sffs_fsinfo_t fsinfo;
//        dev32_t d32;
        int error;

	VBOXVFS_DEBUG(1, "%s: Enter", __FUNCTION__);

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
#if 0
        (void) cmpldev(&d32, vfsp->vfs_dev);
        sbp->f_fsid = d32;
#endif
        sbp->f_namemax = fsinfo.maxnamesize;

	VBOXVFS_DEBUG(1, "%s: Leave", __FUNCTION__);

        return (0);
}
