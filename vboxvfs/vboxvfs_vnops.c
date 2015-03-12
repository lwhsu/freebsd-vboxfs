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
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/kernel.h>
#include <sys/types.h>
#include <sys/malloc.h>
#include <sys/stat.h>
#include <sys/bio.h>
#include <sys/conf.h>
#include <sys/buf.h>
#include <sys/iconv.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/dirent.h>
#include <sys/queue.h>
#include <sys/unistd.h>
#include <sys/endian.h>

#include <vm/uma.h>

#include "vboxvfs.h"

/*
 * For now we'll use an I/O buffer that doesn't page fault for VirtualBox
 * to transfer data into.
 */
char *vboxfs_buffer;

/*
 * Prototypes for VBOXVFS vnode operations
 */
static vop_create_t	vboxfs_create;
static vop_mknod_t	vboxfs_mknod;
static vop_open_t	vboxfs_open;
static vop_close_t	vboxfs_close;
static vop_access_t	vboxfs_access;
static vop_getattr_t	vboxfs_getattr;
static vop_setattr_t	vboxfs_setattr;
static vop_read_t	vboxfs_read;
static vop_write_t	vboxfs_write;
static vop_fsync_t	vboxfs_fsync;
static vop_remove_t	vboxfs_remove;
static vop_link_t	vboxfs_link;
static vop_lookup_t	vboxfs_lookup;
static vop_rename_t	vboxfs_rename;
static vop_mkdir_t	vboxfs_mkdir;
static vop_rmdir_t	vboxfs_rmdir;
static vop_symlink_t	vboxfs_symlink;
static vop_readdir_t	vboxfs_readdir;
static vop_strategy_t	vboxfs_strategy;
static vop_bmap_t	vboxfs_bmap;
static vop_print_t	vboxfs_print;
static vop_pathconf_t	vboxfs_pathconf;
static vop_advlock_t	vboxfs_advlock;
static vop_getextattr_t	vboxfs_getextattr;
static vop_ioctl_t	vboxfs_ioctl;
static vop_getpages_t	vboxfs_getpages;
static vop_inactive_t	vboxfs_inactive;
static vop_putpages_t	vboxfs_putpages;
static vop_reclaim_t	vboxfs_reclaim;
static vop_vptofh_t	vboxfs_vptofh;

struct vop_vector vboxfs_vnodeops = {
	.vop_default	= &default_vnodeops,

	.vop_access	= vboxfs_access,
	.vop_bmap	= vboxfs_bmap,
	.vop_advlock	= vboxfs_advlock,
	.vop_close	= vboxfs_close,
	.vop_create	= vboxfs_create,
	.vop_fsync	= vboxfs_fsync,
	.vop_getattr	= vboxfs_getattr,
	.vop_getextattr = vboxfs_getextattr,
	.vop_getpages	= vboxfs_getpages,
	.vop_inactive	= vboxfs_inactive,
	.vop_ioctl	= vboxfs_ioctl,
	.vop_link	= vboxfs_link,
	.vop_lookup	= vboxfs_lookup,
	.vop_mkdir	= vboxfs_mkdir,
	.vop_mknod	= vboxfs_mknod,
	.vop_open	= vboxfs_open,
	.vop_pathconf	= vboxfs_pathconf,
	.vop_print	= vboxfs_print,
	.vop_putpages	= vboxfs_putpages,
	.vop_read	= vboxfs_read,
	.vop_readdir	= vboxfs_readdir,
	.vop_reclaim	= vboxfs_reclaim,
	.vop_remove	= vboxfs_remove,
	.vop_rename	= vboxfs_rename,
	.vop_rmdir	= vboxfs_rmdir,
	.vop_setattr	= vboxfs_setattr,
	.vop_strategy	= vboxfs_strategy,
	.vop_vptofh 	= vboxfs_vptofh,
	.vop_symlink	= vboxfs_symlink,
	.vop_write	= vboxfs_write,
};

int
vboxfs_allocv(struct mount *mp, struct vnode **vpp, struct thread *td)
{
	int error;
	struct vnode *vp;

	error = getnewvnode("vboxfs", mp, &vboxfs_vnodeops, &vp);
	if (error) {
		printf("vboxfs_allocv: failed to allocate new vnode\n");
		return (error);
	}

	*vpp = vp;
	return (0);
}

/*
 * uid and gid in sffs determine owner and group for all files.
 */
#if 0
static int
sfnode_access(sfnode_t *node, mode_t mode, cred_t *cr)
{
	sffs_data_t *sffs = node->sf_sffs;
	mode_t m;
	int shift = 0;
	int error;
	vnode_t *vp;

	ASSERT(MUTEX_HELD(&sffs_lock));

	/* get the mode from the cache or provider */
	if (sfnode_stat_cached(node))
		error = 0;
	else
		error = sfnode_update_stat_cache(node);
	m = (error == 0) ? node->sf_stat.sf_mode : 0;

	/*
	 * mask off the permissions based on uid/gid
	 */
 	if (crgetuid(cr) != sffs->sf_uid) {
		shift += 3;
		if (groupmember(sffs->sf_gid, cr) == 0)
			shift += 3;
	}
	mode &= ~(m << shift);

	if (mode == 0) {
		error = 0;
	} else {
		vp = sfnode_get_vnode(node);
		error = secpolicy_vnode_access(cr, vp, sffs->sf_uid, mode);
		VN_RELE(vp);
	}
	return (error);
}
#endif 

static int
vboxfs_access(struct vop_access_args *ap)
{
#if 0
	sfnode_t *node = VN2SFN(vp->a_vp);
	int error;

	error = sfnode_access(node, mode, cr);

	return (error);
#endif 
	struct vnode *vp = ap->a_vp;
	accmode_t accmode = ap->a_accmode;
   
	if ((accmode & VWRITE) && (vp->v_mount->mnt_flag & MNT_RDONLY)) {
		switch (vp->v_type) {
		case VDIR:
		case VLNK:
		case VREG:
			return (EROFS);
			/* NOT REACHED */
		default:
			break;
		}
	}
	return (vaccess(vp->v_type, 0444, 0, 0,
	    accmode, ap->a_cred, NULL));
}

/*
 * Clears the (cached) directory listing for the node.
 */
static void
vfsnode_clear_dir_list(struct vboxfs_node *np)
{
	while (np->sf_dir_list != NULL) {
		sffs_dirents_t *next = np->sf_dir_list->sf_next;
		free(np->sf_dir_list, M_VBOXVFS);
		np->sf_dir_list = next;
	}
}

/*
 * Open the provider file associated with a vnode. Holding the file open is
 * the only way we have of trying to have a vnode continue to refer to the
 * same host file in the host in light of the possibility of host side renames.
 */
static void
vfsnode_open(struct vboxfs_node *np)
{
	int error;
	sfp_file_t *fp;

	if (np->sf_file != NULL)
		return;
	error = sfprov_open(np->vboxfsmp->sf_handle, np->sf_path, &fp);
	if (error == 0)
		np->sf_file = fp;
}

/*
 * get a new vnode reference for an sfnode
 */
#if 0
struct vnode *
vfsnode_get_vnode(struct vboxfs_node *node)
{
	struct vnode *vp;

	if (node->sf_vnode != NULL) {
		vref(node->sf_vnode);
	} else {
		vp = vn_alloc(KM_SLEEP);
		printf("  %s gets vnode 0x%p\n", node->sf_path, vp);
		vp->v_type = node->sf_type;
		vp->v_vfsp = node->sf_sffs->sf_vfsp;
		vn_setops(vp, sffs_ops);
		vp->v_flag = VNOSWAP;
#ifndef VBOXVFS_WITH_MMAP
		vp->v_flag |= VNOMAP;
#endif
		vn_exists(vp);
		vp->v_data = node;
		node->sf_vnode = vp;
	}
	return (node->sf_vnode);
}
#endif

static int
vboxfs_open(struct vop_open_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vboxfs_node *np;
	int error = 0;
	off_t fsize;

	np = VTOVBOXFS(vp);
	vfsnode_open(np);
	if (np->sf_file == NULL)
		error = EINVAL;

	if (error == 0) {
		fsize = np->vboxfsmp->size;
		vnode_create_vobject(ap->a_vp, fsize, ap->a_td);
	}

	return (error);
}

#if 0
/*
 * Some sort of host operation on an vboxfs_node has failed or it has been
 * deleted. Mark this node and any children as stale, deleting knowledge
 * about any which do not have active vnodes or children
 * This also handle deleting an inactive node that was already stale.
 */
static void
fvsnode_make_stale(struct vboxfs_node *node)
{
	struct vboxfs_node *np;
	int len;
	avl_index_t where;

	/*
	 * First deal with any children of a directory node.
	 * If a directory becomes stale, anything below it becomes stale too.
	 */
	if (!node->sf_is_stale && node->sf_type == VDIR) {
		len = strlen(node->sf_path);

		np = node;
		while ((np = AVL_NEXT(&sfnodes, node)) != NULL) {
			ASSERT(!n->sf_is_stale);

			/*
			 * quit when no longer seeing children of node
			 */
			if (n->sf_sffs != node->sf_sffs ||
			    strncmp(node->sf_path, n->sf_path, len) != 0 ||
			    n->sf_path[len] != '/')
				break;

			/*
			 * Either mark the child as stale or destroy it
			 */
			if (n->sf_vnode == NULL && n->sf_children == 0) {
				sfnode_destroy(n);
			} else {
				LogFlowFunc(("sffs_make_stale(%s) sub\n",
				    n->sf_path));
				sfnode_clear_dir_list(n);

				if (avl_find(&sfnodes, n, &where) == NULL)
					panic("sfnode_make_stale(%s)"
					    " not in sfnodes", n->sf_path);
				avl_remove(&sfnodes, n);
				n->sf_is_stale = 1;
				if (avl_find(&stale_sfnodes, n, &where) != NULL)
					panic("sffs_make_stale(%s) duplicates",
					    n->sf_path);
				avl_insert(&stale_sfnodes, n, where);
			}
		}
	}
	
	/*
	 * Now deal with the given node.
	 */
	if (node->sf_vnode == NULL && node->sf_children == 0) {
		sfnode_destroy(node);
	} else if (!node->sf_is_stale) {
		LogFlowFunc(("sffs_make_stale(%s)\n", node->sf_path));
		sfnode_clear_dir_list(node);
		if (node->sf_parent)
			sfnode_clear_dir_list(node->sf_parent);
		if (avl_find(&sfnodes, node, &where) == NULL)
			panic("sfnode_make_stale(%s) not in sfnodes",
			    node->sf_path);
		avl_remove(&sfnodes, node);
		node->sf_is_stale = 1;
		if (avl_find(&stale_sfnodes, node, &where) != NULL)
			panic("sffs_make_stale(%s) duplicates", node->sf_path);
		avl_insert(&stale_sfnodes, node, where);
	}
}
static uint64_t
sfnode_cur_time_usec(void)
{
	clock_t now = drv_hztousec(ddi_get_lbolt());
	return now;
}

static int
sfnode_stat_cached(sfnode_t *node)
{
	return (sfnode_cur_time_usec() - node->sf_stat_time) <
	    node->sf_sffs->sf_stat_ttl * 1000L;
}
#endif

static void
vfsnode_invalidate_stat_cache(struct vboxfs_node *np)
{
	np->sf_stat_time = 0;
}

static int
vboxfs_close(struct vop_close_args *ap)
{
	
	struct vnode *vp = ap->a_vp;
	struct vboxfs_node *np;

	np = VTOVBOXFS(vp);

	/*
	 * Free the directory entries for the node. We do this on this call
	 * here because the directory node may not become inactive for a long
	 * time after the readdir is over. Case in point, if somebody cd's into
	 * the directory then it won't become inactive until they cd away again.
	 * In such a case we would end up with the directory listing not getting
	 * updated (i.e. the result of 'ls' always being the same) until they
	 * change the working directory.
	 */
	vfsnode_clear_dir_list(np);

	vfsnode_invalidate_stat_cache(np);

	if (np->sf_file != NULL) {
		(void) sfprov_close(np->sf_file);
		np->sf_file = NULL;
	}

	return (0);
}

static int
vboxfs_getattr(struct vop_getattr_args *ap)
{
	struct vnode 		*vp = ap->a_vp;
	struct vattr 		*vap = ap->a_vap;
	struct vboxfs_node	*np = VTOVBOXFS(vp);
	struct vboxfs_mnt  	*mp = np->vboxfsmp;
	mode_t			mode;
	int			error = 0;

	vap->va_type = vp->v_type;
	
	vap->va_nlink = 1;		/* number of references to file */
	vap->va_uid = mp->sf_uid;	/* owner user id */
	vap->va_gid = mp->sf_gid;	/* owner group id */
	vap->va_rdev = NODEV;		/* device the special file represents */ 
	vap->va_gen = VNOVAL;		/* generation number of file */
	vap->va_flags = 0;		/* flags defined for file */
	vap->va_filerev = 0;		/* file modification number */
	vap->va_vaflags = 0;		/* operations flags */
	vap->va_fileid = np->sf_ino;	/* file id */
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	if (vap->va_fileid == 0)
		vap->va_fileid = 2;
#if 0
	if (!sfnode_stat_cached(node)) {
		error = sfnode_update_stat_cache(node);
		if (error != 0)
		goto done;
	}

	vap->va_atime = node->sf_stat.sf_atime;
	vap->va_mtime = node->sf_stat.sf_mtime;
	vap->va_ctime = node->sf_stat.sf_ctime;
#endif
	vap->va_atime.tv_sec = VNOVAL;
	vap->va_atime.tv_nsec = VNOVAL;
	vap->va_mtime.tv_sec = VNOVAL;
	vap->va_mtime.tv_nsec = VNOVAL;
	vap->va_ctime.tv_sec = VNOVAL;
	vap->va_ctime.tv_nsec = VNOVAL;

	mode = np->sf_stat.sf_mode;
#if 0
	vap->va_mode = mode & MODEMASK;	/* files access mode and type */
	if (S_ISDIR(mode)) {
		vap->va_type = VDIR;	/* vnode type (for create) */
		vap->va_mode = mp->sf_dmode != ~0 ? (mp->sf_dmode & 0777) : vap->va_mode;
		vap->va_mode &= ~mp->sf_dmask;
		vap->va_mode |= S_IFDIR;
	} else if (S_ISREG(mode)) {
		vap->va_type = VREG;
		vap->va_mode = mp->sf_fmode != ~0 ? (mp->sf_fmode & 0777) : vap->va_mode;
		vap->va_mode &= ~mp->sf_fmask;
		vap->va_mode |= S_IFREG;
	} else if (S_ISFIFO(mode))
		vap->va_type = VFIFO;
	else if (S_ISCHR(mode))
		vap->va_type = VCHR;
	else if (S_ISBLK(mode))
		vap->va_type = VBLK;
	else if (S_ISLNK(mode)) {
		vap->va_type = VLNK;
		vap->va_mode = mp->sf_fmode != ~0 ? (mp->sf_fmode & 0777) : vap->va_mode;
		vap->va_mode &= ~mp->sf_fmask;
		vap->va_mode |= S_IFLNK;
	} else if (S_ISSOCK(mode))
		vap->va_type = VSOCK;
#endif
	if (vp->v_type & VDIR) {
		vap->va_nlink = 2;
		vap->va_mode = 0555;
	} else {
		vap->va_nlink = 1;
		vap->va_mode = 0444;
	}

	vap->va_size = np->sf_stat.sf_size;
	vap->va_blocksize = 512;
	/* bytes of disk space held by file */
   	vap->va_bytes = (np->sf_stat.sf_alloc + 511) / 512;

//done:
	return (error);
}

static int
vboxfs_setattr(struct vop_setattr_args *ap)
{
	
	struct vnode 		*vp = ap->a_vp;
	struct vattr 		*vap = ap->a_vap;
	struct vboxfs_node	*np = VTOVBOXFS(vp);
	int			error;
	mode_t			mode;

	mode = vap->va_mode;
	if (vp->v_type == VREG)
		mode |= S_IFREG;
	else if (vp->v_type == VDIR)
		mode |= S_IFDIR;
	else if (vp->v_type == VBLK)
		mode |= S_IFBLK;
	else if (vp->v_type == VCHR)
		mode |= S_IFCHR;
	else if (vp->v_type == VLNK)
		mode |= S_IFLNK;
	else if (vp->v_type == VFIFO)
		mode |= S_IFIFO;
	else if (vp->v_type == VSOCK)
		mode |= S_IFSOCK;

	vfsnode_invalidate_stat_cache(np);
	error = sfprov_set_attr(np->vboxfsmp->sf_handle, np->sf_path,
	    mode, vap->va_atime, vap->va_mtime, vap->va_ctime);
#if 0
	if (error == ENOENT)
		sfnode_make_stale(np);
#endif
	if (vap->va_flags != (u_long)VNOVAL || vap->va_uid != (uid_t)VNOVAL ||
	    vap->va_gid != (gid_t)VNOVAL || vap->va_atime.tv_sec != VNOVAL ||
	    vap->va_mtime.tv_sec != VNOVAL || vap->va_mode != (mode_t)VNOVAL)
		return (EROFS);
	if (vap->va_size != (u_quad_t)VNOVAL) {
		switch (vp->v_type) {
		case VDIR:
			return (EISDIR);
		case VLNK:
		case VREG:
			return (EROFS);
		case VCHR:
		case VBLK:
		case VSOCK:
		case VFIFO:
		case VNON:
		case VBAD:
		case VMARKER:
			return (0);
		}
	}
	return (error);
}

#define blkoff(vboxfsmp, loc)	((loc) & (vboxfsmp)->bmask)

static int
vboxfs_read(struct vop_read_args *ap)
{
	static const int clustersize = MAXBSIZE;	

	struct vnode		*vp = ap->a_vp;
	struct uio 		*uio = ap->a_uio;
	struct vboxfs_node	*np = VTOVBOXFS(vp);
	int			error = 0;
	uint32_t		bytes;
	uint32_t		done;
	unsigned long		offset;
	ssize_t			total;
	long 			on;

	if (vp->v_type == VDIR)
		return (EISDIR);
	if (vp->v_type != VREG)
		return (EINVAL);
#if 0
	if (uio->uio_loffset >= MAXOFFSET_T) {
		proc_t *p = ttoproc(curthread);
		(void) rctl_action(rctlproc_legacy[RLIMIT_FSIZE], p->p_rctls,
		    p, RCA_UNSAFE_SIGINFO);
		return (EFBIG);
	}
	if (uio->uio_loffset < 0)
		return (EINVAL);
#endif
	total = uio->uio_resid;
	if (total == 0)
		return (0);

	vfsnode_open(np);
	if (np->sf_file == NULL)
		return (EINVAL);

	do {
		offset = uio->uio_offset;
		on = blkoff(np->vboxfsmp, uio->uio_offset);
		done = bytes = min((u_int)(clustersize - on), uio->uio_resid);
		error = sfprov_read(np->sf_file, vboxfs_buffer, offset, &done);
		if (error == 0 && done > 0)
			error = uiomove(vboxfs_buffer, done, uio);
	} while (error == 0 && uio->uio_resid > 0 && done > 0);


	/*
	 * a partial read is never an error
	 */
	if (total != uio->uio_resid)
		error = 0;
	return (error);
}

static int
vboxfs_write(struct vop_write_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_create(struct vop_create_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_remove(struct vop_remove_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_rename(struct vop_rename_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_link(struct vop_link_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_symlink(struct vop_symlink_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_mknod(struct vop_mknod_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_mkdir(struct vop_mkdir_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_rmdir(struct vop_rmdir_args *ap)
{
	return (EOPNOTSUPP);
}
#if 0
struct vboxfs_uiodir {
	struct dirent *dirent;
	u_long *cookies;
	int ncookies;
	int acookies;
	int eofflag;
};

static int
vboxfs_uiodir(struct vboxfs_uiodir *uiodir, int de_size, struct uio *uio,
    long cookie)
{
	if (uiodir->cookies != NULL) {
		if (++uiodir->acookies > uiodir->ncookies) {
			uiodir->eofflag = 0;
			return (-1);
		}
		*uiodir->cookies++ = cookie;
	}

	if (uio->uio_resid < de_size) {
		uiodir->eofflag = 0;
		return (-1);
	}

	return (uiomove(uiodir->dirent, de_size, uio));
}
#endif
static int vboxfs_readdir(struct vop_readdir_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct vboxfs_node *dir = VTOVBOXFS(vp);
	struct vboxfs_node *node;
	struct sffs_dirent *dirent = NULL;
	sffs_dirents_t *cur_buf;
	off_t offset = 0;
	//off_t orig_off = uio->uio_offset;
	int error = 0;
#if 0
	if (uio->uio_iovcnt != 1)
		return (EINVAL);

	if (vp->v_type != VDIR)
		return (ENOTDIR);

	if (eofp == NULL)
		eofp = &dummy_eof;
	*eofp = 0;

	if (uio->uio_loffset >= MAXOFFSET_T) {
		*eofp = 1;
		return (0);
	}
#endif
	/*
	 * Get the directory entry names from the host. This gets all
	 * entries. These are stored in a linked list of sffs_dirents_t
	 * buffers, each of which contains a list of dirent64_t's.
	 */

	if (dir->sf_dir_list == NULL) {
		error = sfprov_readdir(dir->vboxfsmp->sf_handle, dir->sf_path,
		    &dir->sf_dir_list);
		if (error != 0)
			goto done;
	}

	/*
	 * Validate and skip to the desired offset.
	 */
	cur_buf = dir->sf_dir_list;
	offset = 0;

	while (cur_buf != NULL && offset + cur_buf->sf_len <= uio->uio_offset) {
		offset += cur_buf->sf_len;
		cur_buf = cur_buf->sf_next;
	}
	
	if (cur_buf == NULL && offset != uio->uio_offset) {
		error = EINVAL;
		goto done;
	}

	if (cur_buf != NULL && offset != uio->uio_offset) {
		off_t off = offset;
		int step;
		dirent = &cur_buf->sf_entries[0];

		while (off < uio->uio_offset) {
			step = sizeof(sffs_stat_t) + dirent->sf_entry.d_reclen;
			dirent = (struct sffs_dirent *) (((char *) dirent) + step);
			off += step;
		}

		if (off >= uio->uio_offset) {
			error = EINVAL;
			goto done;
		}
	}

	offset = uio->uio_offset - offset;

	/*
	 * Lookup each of the names, so that we have ino's, and copy to
	 * result buffer.
	 */
	while (cur_buf != NULL) {
		if (offset >= cur_buf->sf_len) {
			cur_buf = cur_buf->sf_next;
			offset = 0;
			continue;
		}

		dirent = (struct sffs_dirent *)
		    (((char *) &cur_buf->sf_entries[0]) + offset);
		if (dirent->sf_entry.d_reclen > uio->uio_resid)
			break;

		if (strcmp(dirent->sf_entry.d_name, ".") == 0) {
			node = dir;
		} else if (strcmp(dirent->sf_entry.d_name, "..") == 0) {
			node = dir->sf_parent;
			if (node == NULL)
				node = dir;
		} else {
#if 0
		node = sfnode_lookup(dir, dirent->sf_entry.d_name, VNON,
		    0, &dirent->sf_stat, sfnode_cur_time_usec(), NULL);
		if (node == NULL)
			panic("sffs_readdir() lookup failed");
#endif
		}

		error = uiomove(&dirent->sf_entry, dirent->sf_entry.d_reclen, uio);
		if (error != 0)
			break;

		uio->uio_offset = dirent->sf_entry.d_fileno;
		offset += sizeof(sffs_stat_t) + dirent->sf_entry.d_reclen;
	}
done:
#if 0
	if (error != 0)
		uio->uio_offset = orig_off;
#endif
	return (error);
}

static int
vboxfs_fsync(struct vop_fsync_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_print (struct vop_print_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vboxfs_node *np;

	np = VTOVBOXFS(vp);

	if (np == NULL) {
		printf("No vboxfs_node data\n");
		return (0);
	}

	printf("\tpath = %s, parent = %p", np->sf_path,
	    np->sf_parent ? np->sf_parent : NULL);
	printf("\n");
	return (0);
}

static int
vboxfs_pathconf (struct vop_pathconf_args *ap)
{
	//struct vnode 		*vp = ap->a_vp;
	register_t *retval = ap->a_retval;
	int error = 0;

	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*retval = 65535;
		break;
	case _PC_NAME_MAX:
		*retval = NAME_MAX;
		break;
	case _PC_PATH_MAX:
		*retval = PATH_MAX; 
		break;
	default:
		error = EINVAL;
		break;
	}
	return (error);
}

static int
vboxfs_strategy (struct vop_strategy_args *ap)
{
	struct buf *bp;
	struct vnode *vp;
	struct vboxfs_node *np;
	struct bufobj *bo;

	bp = ap->a_bp;
	vp = ap->a_vp;
	np = VTOVBOXFS(vp);

	if (bp->b_blkno == bp->b_lblkno) {
		bp->b_blkno = bp->b_lblkno << (np->vboxfsmp->bshift - DEV_BSHIFT);
	}
	bo = np->vboxfsmp->sf_bo;
	bp->b_iooffset = dbtob(bp->b_blkno);
	BO_STRATEGY(bo, bp);
	return (0);
}

static int
vboxfs_bmap(struct vop_bmap_args *ap)
{
	struct vnode *vp;	
	struct vboxfs_node *np;
	
	vp = ap->a_vp;
	np = VTOVBOXFS(vp);

	if (ap->a_bop != NULL)
		*ap->a_bop = &np->vboxfsmp->sf_devvp->v_bufobj;
	if (ap->a_bnp == NULL)
		return (0);
	if (ap->a_runb)
		*ap->a_runb = 0;

	/* Translate logical to physical sector number */
	*ap->a_bnp = ap->a_bn << (np->vboxfsmp->bshift - DEV_BSHIFT);

	if (ap->a_runp)
		*ap->a_runp = 0;
	if (ap->a_runb)
		*ap->a_runb = 0;
	return (0);
}

/*
 * File specific ioctls.
 */
static int
vboxfs_ioctl(struct vop_ioctl_args *ap)
{
	return (ENOTTY);
}

static int
vboxfs_getextattr(struct vop_getextattr_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_advlock(struct vop_advlock_args *ap)
{
	return (EOPNOTSUPP);
}

/*
 * Look for a cached node, if not found either handle ".." or try looking
 * via the provider. Create an entry in sfnodes if found but not cached yet.
 * If the create flag is set, a file or directory is created. If the file
 * already existed, an error is returned.
 * Nodes returned from this routine always have a vnode with its ref count
 * bumped by 1.
 */
#if 0
static struct vboxfs_node *
vfsnode_lookup(struct vboxfs_node *dir, char *name, vtype_t create,
    mode_t c_mode, sffs_stat_t *stat, uint64_t stat_time, int *err)
{
	avl_index_t	 where;
	vboxfs_node	 template;
	vboxfs_node	*node;
	int		 error = 0;
	int		 type;
	char		*fullpath;
	sfp_file_t	*fp;
	sffs_stat_t	 tmp_stat;

	if (err)
		*err = error;

	/* handle referencing myself */
	if (strcmp(name, "") == 0 || strcmp(name, ".") == 0)
		return (dir);

	/* deal with parent */
	if (strcmp(name, "..") == 0)
		return (dir->sf_parent);

	/*
	 * Look for an existing node.
	 */
	fullpath = sfnode_construct_path(dir, name);
	template.sf_sffs = dir->sf_sffs;
	template.sf_path = fullpath;
	template.sf_is_stale = 0;
	node = avl_find(&sfnodes, &template, &where);
	if (node != NULL) {
		free(fullpath, M_VBOXVFS);
		if (create != VNON)
			return (NULL);
		return (node);
	}

	/*
	 * No entry for this path currently.
	 * Check if the file exists with the provider and get the type from
	 * there.
	 */
	if (create == VREG) {
		type = VREG;
		stat = &tmp_stat;
		error = sfprov_create(dir->sf_sffs->sf_handle, fullpath, c_mode,
		    &fp, stat);
		stat_time = sfnode_cur_time_usec();
	} else if (create == VDIR) {
		type = VDIR;
		stat = &tmp_stat;
		error = sfprov_mkdir(dir->sf_sffs->sf_handle, fullpath, c_mode,
		    &fp, stat);
		stat_time = sfnode_cur_time_usec();
	} else {
		mode_t m;
		fp = NULL;
		type = VNON;
		if (stat == NULL) {
			stat = &tmp_stat;
			error = sfprov_get_attr(dir->sf_sffs->sf_handle,
			    fullpath, stat);
			stat_time = sfnode_cur_time_usec();
		} else
			error = 0;
		m = stat->sf_mode;
		if (error != 0)
			error = ENOENT;
		else if (S_ISDIR(m))
			type = VDIR;
		else if (S_ISREG(m))
			type = VREG;
		else if (S_ISLNK(m))
			type = VLNK;
	}

	if (err)
		*err = error;

	/*
	 * If no errors, make a new node and return it.
	 */
	if (error) {
		kmem_free(fullpath, strlen(fullpath) + 1);
		return (NULL);
	}
	node = sfnode_make(dir->sf_sffs, fullpath, type, fp, dir, stat,
	    stat_time);
	return (node);
}
#endif

/*
 * Lookup an entry in a directory and create a new vnode if found.
 */	
static int 
vboxfs_lookup(struct vop_lookup_args /* {
		struct vnodeop_desc *a_desc;
		struct vnode *a_dvp;
		struct vnode **a_vpp;
		struct componentname *a_cnp;
	} */ *ap)
{
	struct 	componentname *cnp = ap->a_cnp;
	struct 	vnode *dvp = ap->a_dvp;		/* the directory vnode */
	char	*nameptr = cnp->cn_nameptr;	/* the name of the file or directory */
	struct	vnode **vpp = ap->a_vpp;	/* the vnode we found or NULL */
	struct  vnode *tdp = NULL;
	struct 	vboxfs_node *node = VTOVBOXFS(dvp);
	struct 	vboxfs_mnt *vboxfsmp = node->vboxfsmp;
	u_long  nameiop = cnp->cn_nameiop;
	u_long 	flags = cnp->cn_flags;
	//long 	namelen;
	ino_t 	id = 0;
	int 	ltype, error = 0;
	int 	lkflags = cnp->cn_lkflags;	

	/* dvp must be a directory */
	if (dvp->v_type != VDIR)
		return (ENOTDIR);
	
	if (strcmp(nameptr, THEFILE_NAME) == 0)
		id = THEFILE_INO;
	else if (flags & ISDOTDOT)
		id = ROOTDIR_INO;

	/* Did we have a match? */
	if (id) {
		if (flags & ISDOTDOT) {
			error = vn_vget_ino(dvp, id, lkflags, &tdp);
		} else if (node->sf_ino == id) {
			VREF(dvp);	/* we want ourself, ie "." */
			/*
			 * When we lookup "." we still can be asked to lock it
			 * differently.
			 */
			ltype = lkflags & LK_TYPE_MASK;
			if (ltype != VOP_ISLOCKED(dvp)) {
				if (ltype == LK_EXCLUSIVE)
					vn_lock(dvp, LK_UPGRADE | LK_RETRY);
				else /* if (ltype == LK_SHARED) */
					vn_lock(dvp, LK_DOWNGRADE | LK_RETRY);
			}
			tdp = dvp;
		} else
			error = vboxfs_vget(vboxfsmp->sf_vfsp, id, lkflags, &tdp);
		if (!error) {
			*vpp = tdp;
			/* Put this entry in the cache */
			if (flags & MAKEENTRY)
				cache_enter(dvp, *vpp, cnp);
		}
	} else {
		/* Enter name into cache as non-existant */
		if (flags & MAKEENTRY)
			cache_enter(dvp, *vpp, cnp);

		if ((flags & ISLASTCN) &&
		    (nameiop == CREATE || nameiop == RENAME)) {
			error = EROFS;
		} else {
			error = ENOENT;
		}
	}

	return (error);
}

static int
vboxfs_inactive(struct vop_inactive_args *ap)
{
   	return (0);
}

static int
vboxfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vnode *vp;
	struct vboxfs_node *np;

	vp = ap->a_vp;
	np = VTOVBOXFS(vp);

	/*
	 * Destroy the vm object and flush associated pages.
	 */
	vnode_destroy_vobject(vp);

	if (np != NULL) {
		vfs_hash_remove(vp);
		free(np, M_VBOXVFS);
		vp->v_data = NULL;
	}
	return (0);
}

static int
vboxfs_vptofh(struct vop_vptofh_args *ap)
{
#if 0
	struct vboxfs_node *node;
	struct ifid *ifhp;

	node = VTON(ap->a_vp);
	ifhp = (struct ifid *)ap->a_fhp;
	ifhp->ifid_len = sizeof(struct ifid);
	ifhp->ifid_ino = node->hash_id;
#endif
	return (EOPNOTSUPP);
}

static int
vboxfs_getpages(struct vop_getpages_args *ap)
{
	return (EOPNOTSUPP);
}

static int
vboxfs_putpages(struct vop_putpages_args *ap)
{
	return (EOPNOTSUPP);
}
