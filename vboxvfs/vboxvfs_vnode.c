/*-
 * SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
 *
 * Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
 */

/*
 * vboxvfs -- vnode operations.
 */

#include "vboxvfs_internal.h"

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/dirent.h>
#include <sys/limits.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sx.h>
#include <sys/unistd.h>

static vop_access_t	vboxvfs_access;
static vop_cachedlookup_t vboxvfs_cachedlookup;
static vop_close_t	vboxvfs_close;
static vop_getattr_t	vboxvfs_getattr;
static vop_inactive_t	vboxvfs_inactive;
static vop_lookup_t	vboxvfs_lookup;
static vop_open_t	vboxvfs_open;
static vop_pathconf_t	vboxvfs_pathconf;
static vop_print_t	vboxvfs_print;
static vop_readdir_t	vboxvfs_readdir;
static vop_reclaim_t	vboxvfs_reclaim;
static vop_strategy_t	vboxvfs_strategy;

struct vop_vector vboxvfs_vnodeops = {
	.vop_default =		&default_vnodeops,

	.vop_access =		vboxvfs_access,
	.vop_cachedlookup =	vboxvfs_cachedlookup,
	.vop_close =		vboxvfs_close,
	.vop_getattr =		vboxvfs_getattr,
	.vop_inactive =		vboxvfs_inactive,
	.vop_lookup =		vboxvfs_lookup,
	.vop_open =		vboxvfs_open,
	.vop_pathconf =		vboxvfs_pathconf,
	.vop_print =		vboxvfs_print,
	.vop_readdir =		vboxvfs_readdir,
	.vop_reclaim =		vboxvfs_reclaim,
	.vop_strategy =		vboxvfs_strategy,
};
VFS_VOP_VECTOR_REGISTER(vboxvfs_vnodeops);

/*
 * Name lookup.  The directory's attributes are revalidated first so that a
 * directory changed on the host gets its name cache entries purged; the
 * cache lookup itself and the locking protocol are left to
 * vfs_cache_lookup(), which calls vboxvfs_cachedlookup() on a miss.
 */
static int
vboxvfs_lookup(struct vop_lookup_args *ap)
{
	struct vnode *dvp;
	int error;

	dvp = ap->a_dvp;
	if (dvp->v_type != VDIR)
		return (ENOTDIR);
	error = vboxvfs_node_revalidate(VTOVBOXVFS(dvp), false);
	if (error != 0)
		return (error);
	return (vfs_cache_lookup(ap));
}

/*
 * vn_vget_ino_gen() callback: lock the parent of a directory.
 */
static int
vboxvfs_vget_parent(struct mount *mp __unused, void *arg, int lkflags,
    struct vnode **rvp)
{
	struct vboxvfs_node *np;

	np = arg;
	*rvp = VBOXVFSTOV(np->vn_parent);
	return (vget(*rvp, lkflags));
}

static int
vboxvfs_cachedlookup(struct vop_cachedlookup_args *ap)
{
	struct componentname *cnp;
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *dnp;
	struct vnode *dvp, *vp;
	SHFLFSOBJINFO info;
	char *path;
	size_t pathlen;
	uint64_t flags;
	int error, nameiop;
	bool islast;

	dvp = ap->a_dvp;
	cnp = ap->a_cnp;
	dnp = VTOVBOXVFS(dvp);
	vmp = dnp->vn_vmp;
	flags = cnp->cn_flags;
	nameiop = cnp->cn_nameiop;
	islast = (flags & ISLASTCN) != 0;
	*ap->a_vpp = NULL;

	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "lookup \"%.*s\" in ino %ju op %d\n",
	    (int)cnp->cn_namelen, cnp->cn_nameptr, (uintmax_t)dnp->vn_ino,
	    nameiop);

	if (flags & ISDOTDOT) {
		if (dnp->vn_flags & VBOXVFS_NODE_ROOT) {
			/* namei() handles crossing the mount; be safe. */
			vref(dvp);
			*ap->a_vpp = dvp;
			return (0);
		}
		error = vn_vget_ino_gen(dvp, vboxvfs_vget_parent, dnp,
		    cnp->cn_lkflags, &vp);
		if (error != 0)
			return (error);
		*ap->a_vpp = vp;
		if (flags & MAKEENTRY)
			cache_enter(dvp, vp, cnp);
		return (0);
	}
	if (cnp->cn_namelen == 1 && cnp->cn_nameptr[0] == '.') {
		vref(dvp);
		*ap->a_vpp = dvp;
		return (0);
	}
	if (cnp->cn_namelen > vmp->vm_namemax)
		return (ENAMETOOLONG);

	error = vboxvfs_node_childpath(dnp, cnp->cn_nameptr, cnp->cn_namelen,
	    &path, &pathlen);
	if (error != 0)
		return (error);
	error = vboxvfs_host_lookup(vmp, path, pathlen, &info);
	vboxvfs_node_freepath(path);
	if (error == ENOENT) {
		if (islast && (nameiop == CREATE || nameiop == RENAME)) {
			error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred,
			    curthread);
			if (error != 0)
				return (error);
			return (EJUSTRETURN);
		}
		/*
		 * No negative name cache entries: a file created on the
		 * host must show up at the next lookup.
		 */
		return (ENOENT);
	}
	if (error != 0)
		return (error);

	if (islast && (nameiop == DELETE || nameiop == RENAME)) {
		error = VOP_ACCESS(dvp, VWRITE, cnp->cn_cred, curthread);
		if (error != 0)
			return (error);
	}
	error = vboxvfs_node_get(vmp, dvp, cnp->cn_nameptr, cnp->cn_namelen,
	    &info, &vp);
	if (error != 0)
		return (error);
	KASSERT((cnp->cn_lkflags & LK_TYPE_MASK) == LK_EXCLUSIVE,
	    ("vboxvfs_cachedlookup: shared lookups are not enabled"));
	*ap->a_vpp = vp;
	if (flags & MAKEENTRY)
		cache_enter(dvp, vp, cnp);
	return (0);
}

static int
vboxvfs_access(struct vop_access_args *ap)
{
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *np;
	struct vnode *vp;
	accmode_t accmode;
	mode_t mode;
	int error;

	vp = ap->a_vp;
	np = VTOVBOXVFS(vp);
	vmp = np->vn_vmp;
	accmode = ap->a_accmode;

	if (accmode & VMODIFY_PERMS) {
		switch (vp->v_type) {
		case VDIR:
		case VLNK:
		case VREG:
			if (vp->v_mount->mnt_flag & MNT_RDONLY)
				return (EROFS);
			break;
		default:
			break;
		}
	}
	error = vboxvfs_node_revalidate(np, false);
	if (error != 0)
		return (error);
	VBOXVFS_NODE_LOCK(np);
	mode = vboxvfs_mode(vmp, vp->v_type, np->vn_info.Attr.fMode);
	VBOXVFS_NODE_UNLOCK(np);
	return (vaccess(vp->v_type, mode, vmp->vm_uid, vmp->vm_gid, accmode,
	    ap->a_cred));
}

static int
vboxvfs_getattr(struct vop_getattr_args *ap)
{
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *np;
	struct vnode *vp;
	struct vattr *vap;
	SHFLFSOBJINFO info;
	int error;

	vp = ap->a_vp;
	vap = ap->a_vap;
	np = VTOVBOXVFS(vp);
	vmp = np->vn_vmp;

	error = vboxvfs_node_revalidate(np, false);
	if (error != 0)
		return (error);
	VBOXVFS_NODE_LOCK(np);
	info = np->vn_info;
	VBOXVFS_NODE_UNLOCK(np);

	VATTR_NULL(vap);
	vap->va_type = vp->v_type;
	vap->va_mode = vboxvfs_mode(vmp, vp->v_type, info.Attr.fMode);
	vap->va_nlink = vp->v_type == VDIR ? 2 : 1;
	vap->va_uid = vmp->vm_uid;
	vap->va_gid = vmp->vm_gid;
	vap->va_fsid = vp->v_mount->mnt_stat.f_fsid.val[0];
	vap->va_fileid = np->vn_ino;
	vap->va_size = info.cbObject;
	vap->va_blocksize = vmp->vm_biosize;
	vboxvfs_ns2ts(info.AccessTime.i64NanosecondsRelativeToUnixEpoch,
	    &vap->va_atime);
	vboxvfs_ns2ts(info.ModificationTime.i64NanosecondsRelativeToUnixEpoch,
	    &vap->va_mtime);
	vboxvfs_ns2ts(info.ChangeTime.i64NanosecondsRelativeToUnixEpoch,
	    &vap->va_ctime);
	vboxvfs_ns2ts(info.BirthTime.i64NanosecondsRelativeToUnixEpoch,
	    &vap->va_birthtime);
	vap->va_gen = 0;
	vap->va_flags = 0;
	vap->va_rdev = NODEV;
	vap->va_bytes = info.cbAllocated;
	vap->va_filerev = 0;
	return (0);
}

static int
vboxvfs_open(struct vop_open_args *ap)
{
	struct vboxvfs_node *np;
	struct vnode *vp;
	uint64_t size;
	int error;

	vp = ap->a_vp;
	np = VTOVBOXVFS(vp);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "open ino %ju mode %#x\n",
	    (uintmax_t)np->vn_ino, ap->a_mode);

	/* Close-to-open consistency: always ask the host at open time. */
	error = vboxvfs_node_revalidate(np, true);
	if (error != 0)
		return (error);
	VBOXVFS_NODE_LOCK(np);
	size = np->vn_size;
	np->vn_opencnt++;
	VBOXVFS_NODE_UNLOCK(np);
	if (vp->v_type == VREG)
		vnode_create_vobject(vp, size, ap->a_td);
	return (0);
}

static int
vboxvfs_close(struct vop_close_args *ap)
{
	struct vboxvfs_node *np;
	struct vnode *vp;
	bool last;

	vp = ap->a_vp;
	np = VTOVBOXVFS(vp);
	VBOXVFS_NODE_LOCK(np);
	if (np->vn_opencnt > 0)
		np->vn_opencnt--;
	last = np->vn_opencnt == 0;
	VBOXVFS_NODE_UNLOCK(np);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "close ino %ju last %d\n",
	    (uintmax_t)np->vn_ino, last);
	if (last && vp->v_type == VDIR) {
		sx_xlock(&np->vn_dirsx);
		vboxvfs_node_freedir(np);
		sx_xunlock(&np->vn_dirsx);
	}
	return (0);
}

/*
 * Directory listing.
 *
 * The whole directory is fetched from the host on the first read (offset
 * 0) and converted into an array of struct dirent kept on the node; the
 * uio offset and the cookies are entry indices, so seekdir/telldir and NFS
 * cookies are cheap and stable while the snapshot lives.  The snapshot is
 * dropped at end of directory and on the last close.
 */
static int
vboxvfs_dir_append(struct vboxvfs_node *np, ino_t ino, uint8_t type,
    const char *name, u_int namelen)
{
	struct dirent *dp;
	size_t need, newsize;

	if (namelen > MAXNAMLEN)
		return (ENAMETOOLONG);
	need = _GENERIC_DIRLEN(namelen);
	if (np->vn_dirbuflen + need > np->vn_dirbufsize) {
		newsize = MAX(np->vn_dirbufsize * 2, (size_t)PAGE_SIZE);
		while (np->vn_dirbuflen + need > newsize)
			newsize *= 2;
		np->vn_dirbuf = realloc(np->vn_dirbuf, newsize, M_VBOXVFS,
		    M_WAITOK);
		np->vn_dirbufsize = newsize;
	}
	if (np->vn_dircnt == np->vn_diroffcnt) {
		np->vn_diroffcnt = MAX(np->vn_diroffcnt * 2, 64);
		np->vn_diroffs = realloc(np->vn_diroffs,
		    np->vn_diroffcnt * sizeof(*np->vn_diroffs), M_VBOXVFS,
		    M_WAITOK);
	}
	dp = (struct dirent *)(np->vn_dirbuf + np->vn_dirbuflen);
	dp->d_fileno = ino;
	dp->d_off = np->vn_dircnt + 1;
	dp->d_reclen = need;
	dp->d_type = type;
	dp->d_namlen = namelen;
	bcopy(name, dp->d_name, namelen);
	dirent_terminate(dp);
	np->vn_diroffs[np->vn_dircnt] = np->vn_dirbuflen;
	np->vn_dirbuflen += need;
	np->vn_dircnt++;
	return (0);
}

static uint8_t
vboxvfs_mode2dtype(RTFMODE fmode)
{

	switch (vboxvfs_mode2type(fmode)) {
	case VDIR:
		return (DT_DIR);
	case VLNK:
		return (DT_LNK);
	case VFIFO:
		return (DT_FIFO);
	case VSOCK:
		return (DT_SOCK);
	case VCHR:
		return (DT_CHR);
	case VBLK:
		return (DT_BLK);
	default:
		return (DT_REG);
	}
}

/*
 * The inode number of a directory entry: that of its node if one is
 * cached (it may have been renamed into place), else the hash of its path.
 */
static ino_t
vboxvfs_dir_entry_ino(struct vboxvfs_node *dnp, const char *dirpath,
    size_t dirlen, const char *name, u_int namelen)
{
	struct vnode *vp;
	ino_t ino;

	vboxvfs_node_ref(dnp, name, namelen, &vp);
	if (vp != NULL) {
		ino = VTOVBOXVFS(vp)->vn_ino;
		vrele(vp);
		return (ino);
	}
	return (vboxvfs_node_childino(dirpath, dirlen, name, namelen));
}

static int
vboxvfs_dir_fill(struct vboxvfs_node *np)
{
	struct vboxvfs_mount *vmp;
	SHFLDIRINFO *de;
	SHFLHANDLE handle;
	char *buf, *dirpath, *p, *end;
	size_t cb, dirlen;
	uint32_t lflags, n, i;
	int error;
	bool more;

	vmp = np->vn_vmp;
	buf = NULL;
	dirpath = NULL;
	handle = SHFL_HANDLE_NIL;

	error = vboxvfs_dir_append(np, np->vn_ino, DT_DIR, ".", 1);
	if (error == 0)
		error = vboxvfs_dir_append(np, np->vn_parent->vn_ino, DT_DIR,
		    "..", 2);
	if (error != 0)
		goto out;
	error = vboxvfs_node_path(np, &dirpath, &dirlen);
	if (error != 0)
		goto out;
	buf = vboxvfs_host_alloc(VBOXVFS_DIRBUF_SIZE);
	if (buf == NULL) {
		error = ENOMEM;
		goto out;
	}
	error = vboxvfs_node_opendir(np, &handle);
	if (error != 0)
		goto out;

	lflags = SHFL_LIST_RESTART;
	do {
		error = vboxvfs_host_list(vmp, handle, lflags, buf,
		    VBOXVFS_DIRBUF_SIZE, &n, &more);
		if (error != 0)
			goto out;
		lflags = SHFL_LIST_NONE;
		p = buf;
		end = buf + VBOXVFS_DIRBUF_SIZE;
		for (i = 0; i < n; i++) {
			de = (SHFLDIRINFO *)p;
			if (p + RT_UOFFSETOF(SHFLDIRINFO, name.String) > end ||
			    p + RT_UOFFSETOF(SHFLDIRINFO, name.String) +
			    de->name.u16Size > end) {
				VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR,
				    "LIST returned a truncated entry\n");
				error = EIO;
				goto out;
			}
			cb = RT_UOFFSETOF(SHFLDIRINFO, name.String) +
			    de->name.u16Size;
			p += cb;
			if ((de->name.u16Length == 1 &&
			    de->name.String.ach[0] == '.') ||
			    (de->name.u16Length == 2 &&
			    de->name.String.ach[0] == '.' &&
			    de->name.String.ach[1] == '.'))
				continue;
			if (de->name.u16Length == 0 ||
			    de->name.u16Length > MAXNAMLEN) {
				VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR,
				    "skipping entry with name length %u\n",
				    de->name.u16Length);
				continue;
			}
			error = vboxvfs_dir_append(np,
			    vboxvfs_dir_entry_ino(np, dirpath, dirlen,
			    de->name.String.ach, de->name.u16Length),
			    vboxvfs_mode2dtype(de->Info.Attr.fMode),
			    de->name.String.ach, de->name.u16Length);
			if (error != 0)
				goto out;
		}
	} while (more && n != 0);
out:
	if (handle != SHFL_HANDLE_NIL)
		(void)vboxvfs_host_close(vmp, handle);
	vboxvfs_host_free(buf);
	vboxvfs_node_freepath(dirpath);
	if (error != 0)
		vboxvfs_node_freedir(np);
	return (error);
}

static int
vboxvfs_readdir(struct vop_readdir_args *ap)
{
	struct vboxvfs_node *np;
	struct vnode *vp;
	struct uio *uio;
	struct dirent *dp;
	uint64_t *cookies;
	ssize_t resid;
	u_int i, idx, nfit;
	int error;

	vp = ap->a_vp;
	uio = ap->a_uio;
	np = VTOVBOXVFS(vp);
	cookies = NULL;

	if (vp->v_type != VDIR)
		return (ENOTDIR);
	if (uio->uio_offset < 0 || uio->uio_offset > UINT_MAX)
		return (EINVAL);
	idx = uio->uio_offset;
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "readdir ino %ju offset %u\n",
	    (uintmax_t)np->vn_ino, idx);

	sx_xlock(&np->vn_dirsx);
	if (idx == 0 || np->vn_dirbuf == NULL) {
		vboxvfs_node_freedir(np);
		error = vboxvfs_dir_fill(np);
		if (error != 0) {
			sx_xunlock(&np->vn_dirsx);
			return (error);
		}
	}
	if (idx > np->vn_dircnt) {
		sx_xunlock(&np->vn_dirsx);
		return (EINVAL);
	}

	/* How many entries fit? */
	resid = uio->uio_resid;
	for (nfit = 0, i = idx; i < np->vn_dircnt; i++, nfit++) {
		dp = (struct dirent *)(np->vn_dirbuf + np->vn_diroffs[i]);
		if (dp->d_reclen > resid)
			break;
		resid -= dp->d_reclen;
	}
	if (nfit == 0 && idx < np->vn_dircnt) {
		sx_xunlock(&np->vn_dirsx);
		return (EINVAL);
	}
	if (ap->a_ncookies != NULL) {
		cookies = malloc(MAX(nfit, 1) * sizeof(*cookies), M_TEMP,
		    M_WAITOK);
		*ap->a_ncookies = nfit;
		*ap->a_cookies = cookies;
	}
	error = 0;
	for (i = 0; i < nfit; i++) {
		dp = (struct dirent *)(np->vn_dirbuf +
		    np->vn_diroffs[idx + i]);
		error = uiomove(dp, dp->d_reclen, uio);
		if (error != 0)
			break;
		if (cookies != NULL)
			cookies[i] = idx + i + 1;
	}
	if (error != 0 && cookies != NULL) {
		free(cookies, M_TEMP);
		*ap->a_ncookies = 0;
		*ap->a_cookies = NULL;
	}
	uio->uio_offset = idx + i;
	if (ap->a_eofflag != NULL)
		*ap->a_eofflag = uio->uio_offset >= np->vn_dircnt;
	if (uio->uio_offset >= np->vn_dircnt)
		vboxvfs_node_freedir(np);
	sx_xunlock(&np->vn_dirsx);
	return (error);
}

static int
vboxvfs_inactive(struct vop_inactive_args *ap)
{
	struct vboxvfs_node *np;
	struct vnode *vp;
	bool stale;

	vp = ap->a_vp;
	np = VTOVBOXVFS(vp);
	VBOXVFS_NODE_LOCK(np);
	stale = (np->vn_flags & VBOXVFS_NODE_STALE) != 0;
	VBOXVFS_NODE_UNLOCK(np);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "inactive ino %ju stale %d\n",
	    (uintmax_t)np->vn_ino, stale);
	/*
	 * Nothing refers to the file any more: give the host handle back so
	 * that the host (Windows in particular) can delete or rename it.
	 */
	vboxvfs_node_closehandle(np);
	if (stale)
		vrecycle(vp);
	return (0);
}

static int
vboxvfs_reclaim(struct vop_reclaim_args *ap)
{
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *np;
	struct vnode *vp;

	vp = ap->a_vp;
	np = VTOVBOXVFS(vp);
	vmp = np->vn_vmp;
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "reclaim ino %ju\n",
	    (uintmax_t)np->vn_ino);

	vfs_hash_remove(vp);
	vboxvfs_node_closehandle(np);
	if (np->vn_flags & VBOXVFS_NODE_ROOT)
		vmp->vm_rootnode = NULL;
	vp->v_data = NULL;
	vboxvfs_node_free(np);
	return (0);
}

static int
vboxvfs_pathconf(struct vop_pathconf_args *ap)
{
	struct vboxvfs_mount *vmp;

	vmp = VTOVBOXVFS(ap->a_vp)->vn_vmp;
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = 1;
		return (0);
	case _PC_NAME_MAX:
		*ap->a_retval = vmp->vm_namemax;
		return (0);
	case _PC_SYMLINK_MAX:
		*ap->a_retval = MAXPATHLEN;
		return (0);
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 1;
		return (0);
	case _PC_NO_TRUNC:
		*ap->a_retval = 1;
		return (0);
	case _PC_FILESIZEBITS:
		*ap->a_retval = 64;
		return (0);
	default:
		return (vop_stdpathconf(ap));
	}
}

static int
vboxvfs_print(struct vop_print_args *ap)
{
	struct vboxvfs_node *np;

	np = VTOVBOXVFS(ap->a_vp);
	printf("\tvboxvfs: ino %ju, name \"%s\", handle %#jx, opencnt %d, "
	    "flags %#x, size %ju\n", (uintmax_t)np->vn_ino, np->vn_name,
	    (uintmax_t)np->vn_handle, np->vn_opencnt, np->vn_flags,
	    (uintmax_t)np->vn_size);
	return (0);
}

/*
 * The buffer cache path is not implemented yet; fail cleanly instead of
 * falling into vop_nostrategy().
 */
static int
vboxvfs_strategy(struct vop_strategy_args *ap)
{
	struct buf *bp;

	bp = ap->a_bp;
	bp->b_ioflags |= BIO_ERROR;
	bp->b_error = EOPNOTSUPP;
	bufdone(bp);
	return (0);
}
