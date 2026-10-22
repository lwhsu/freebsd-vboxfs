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
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sx.h>
#include <sys/unistd.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>

static vop_access_t	vboxvfs_access;
static vop_cachedlookup_t vboxvfs_cachedlookup;
static vop_close_t	vboxvfs_close;
static vop_create_t	vboxvfs_create;
static vop_getattr_t	vboxvfs_getattr;
static vop_inactive_t	vboxvfs_inactive;
static vop_lookup_t	vboxvfs_lookup;
static vop_mkdir_t	vboxvfs_mkdir;
static vop_open_t	vboxvfs_open;
static vop_pathconf_t	vboxvfs_pathconf;
static vop_print_t	vboxvfs_print;
static vop_readdir_t	vboxvfs_readdir;
static vop_readlink_t	vboxvfs_readlink;
static vop_reclaim_t	vboxvfs_reclaim;
static vop_remove_t	vboxvfs_remove;
static vop_rename_t	vboxvfs_rename;
static vop_rmdir_t	vboxvfs_rmdir;
static vop_setattr_t	vboxvfs_setattr;
static vop_symlink_t	vboxvfs_symlink;

struct vop_vector vboxvfs_vnodeops = {
	.vop_default =		&default_vnodeops,

	.vop_access =		vboxvfs_access,
	.vop_cachedlookup =	vboxvfs_cachedlookup,
	.vop_close =		vboxvfs_close,
	.vop_create =		vboxvfs_create,
	.vop_fsync =		vboxvfs_fsync,
	.vop_getattr =		vboxvfs_getattr,
	.vop_inactive =		vboxvfs_inactive,
	.vop_lookup =		vboxvfs_lookup,
	.vop_mkdir =		vboxvfs_mkdir,
	.vop_open =		vboxvfs_open,
	.vop_pathconf =		vboxvfs_pathconf,
	.vop_print =		vboxvfs_print,
	.vop_readdir =		vboxvfs_readdir,
	.vop_readlink =		vboxvfs_readlink,
	.vop_reclaim =		vboxvfs_reclaim,
	.vop_remove =		vboxvfs_remove,
	.vop_rename =		vboxvfs_rename,
	.vop_rmdir =		vboxvfs_rmdir,
	.vop_setattr =		vboxvfs_setattr,
	.vop_read =		vboxvfs_read,
	.vop_strategy =		vboxvfs_strategy,
	.vop_write =		vboxvfs_write,
	.vop_symlink =		vboxvfs_symlink,
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
	SHFLHANDLE handle;
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
	/*
	 * Open the host handle now with the access that was asked for, so
	 * that e.g. O_WRONLY on a file the host will not let us write fails
	 * here and not at the first write.
	 */
	if (vp->v_type == VREG) {
		error = vboxvfs_node_handle(np, (ap->a_mode & FWRITE) ?
		    SHFL_CF_ACCESS_WRITE : SHFL_CF_ACCESS_READ, &handle);
		if (error != 0)
			return (error);
	}
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
/*
 * Vnodes referenced while building a listing; they are released only after
 * vn_dirsx is dropped, because vrele() may need the child's vnode lock
 * (inactive processing) and that must not be taken under the sx lock.
 */
struct vboxvfs_dirheld {
	struct vnode	**vps;
	u_int		  n;
	u_int		  cap;
};

static ino_t
vboxvfs_dir_entry_ino(struct vboxvfs_node *dnp, const char *dirpath,
    size_t dirlen, const char *name, u_int namelen,
    struct vboxvfs_dirheld *held)
{
	struct vnode *vp;

	vboxvfs_node_ref(dnp, name, namelen, &vp);
	if (vp != NULL) {
		if (held->n == held->cap) {
			held->cap = MAX(held->cap * 2, 64);
			held->vps = realloc(held->vps,
			    held->cap * sizeof(*held->vps), M_VBOXVFS, M_WAITOK);
		}
		held->vps[held->n++] = vp;
		return (VTOVBOXVFS(vp)->vn_ino);
	}
	return (vboxvfs_node_childino(dirpath, dirlen, name, namelen));
}

static void
vboxvfs_dirheld_release(struct vboxvfs_dirheld *held)
{
	u_int i;

	for (i = 0; i < held->n; i++)
		vrele(held->vps[i]);
	free(held->vps, M_VBOXVFS);
	held->vps = NULL;
	held->n = held->cap = 0;
}

static int
vboxvfs_dir_fill(struct vboxvfs_node *np, struct vboxvfs_dirheld *held)
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
			    de->name.String.ach, de->name.u16Length, held),
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
	struct vboxvfs_dirheld held;
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

	held.vps = NULL;
	held.n = held.cap = 0;
	sx_xlock(&np->vn_dirsx);
	if (idx == 0 || np->vn_dirbuf == NULL) {
		vboxvfs_node_freedir(np);
		error = vboxvfs_dir_fill(np, &held);
		if (error != 0) {
			sx_xunlock(&np->vn_dirsx);
			vboxvfs_dirheld_release(&held);
			return (error);
		}
	}
	if (idx > np->vn_dircnt) {
		sx_xunlock(&np->vn_dirsx);
		vboxvfs_dirheld_release(&held);
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
		vboxvfs_dirheld_release(&held);
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
	vboxvfs_dirheld_release(&held);
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

	if ((np->vn_flags & VBOXVFS_NODE_UNHASHED) == 0)
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
 * Modifying operations.
 */

static int
vboxvfs_create_result(SHFLCREATERESULT result)
{

	switch (result) {
	case SHFL_FILE_EXISTS:
		return (EEXIST);
	case SHFL_FILE_NOT_FOUND:
	case SHFL_PATH_NOT_FOUND:
		return (ENOENT);
	default:
		return (EIO);
	}
}

static RTFMODE
vboxvfs_type2rtfs(__enum_uint8(vtype) type)
{

	switch (type) {
	case VDIR:
		return (RTFS_TYPE_DIRECTORY);
	case VLNK:
		return (RTFS_TYPE_SYMLINK);
	default:
		return (RTFS_TYPE_FILE);
	}
}

/*
 * The host creates objects with its own umask and, for directories, a
 * hardcoded 0777; set the mode we were asked for.  Best effort: the host
 * forces u+r (u+rx for directories) and clears suid/sgid/sticky anyway.
 */
static void
vboxvfs_fixmode(struct vboxvfs_mount *vmp, SHFLHANDLE handle, RTFMODE type,
    mode_t mode, SHFLFSOBJINFO *info)
{
	SHFLFSOBJINFO si;

	RT_ZERO(si);
	si.Attr.fMode = type | (mode & ALLPERMS);
	si.Attr.enmAdditional = SHFLFSOBJATTRADD_NOTHING;
	if (vboxvfs_host_setinfo(vmp, handle, &si) == 0)
		info->Attr.fMode = si.Attr.fMode;
}

static int
vboxvfs_create(struct vop_create_args *ap)
{
	struct componentname *cnp;
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *dnp, *np;
	struct vnode *dvp, *vp;
	SHFLCREATEPARMS parms;
	char *path;
	size_t pathlen;
	mode_t mode;
	int error;

	dvp = ap->a_dvp;
	cnp = ap->a_cnp;
	dnp = VTOVBOXVFS(dvp);
	vmp = dnp->vn_vmp;
	if (ap->a_vap->va_type != VREG)
		return (EOPNOTSUPP);
	mode = ap->a_vap->va_mode & ALLPERMS;
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "create \"%.*s\" mode %#o in %ju\n",
	    (int)cnp->cn_namelen, cnp->cn_nameptr, mode, (uintmax_t)dnp->vn_ino);

	error = vboxvfs_node_childpath(dnp, cnp->cn_nameptr, cnp->cn_namelen,
	    &path, &pathlen);
	if (error != 0)
		return (error);
	error = vboxvfs_host_create(vmp, path, pathlen,
	    SHFL_CF_ACT_CREATE_IF_NEW | SHFL_CF_ACT_FAIL_IF_EXISTS |
	    SHFL_CF_ACCESS_READWRITE | SHFL_CF_ACCESS_DENYNONE, mode, &parms);
	vboxvfs_node_freepath(path);
	if (error != 0)
		return (error);
	if (parms.Handle == SHFL_HANDLE_NIL)
		return (vboxvfs_create_result(parms.Result));
	vboxvfs_node_invalattr(dnp);
	if ((parms.Info.Attr.fMode & ALLPERMS) != mode)
		vboxvfs_fixmode(vmp, parms.Handle, RTFS_TYPE_FILE, mode,
		    &parms.Info);

	error = vboxvfs_node_get(vmp, dvp, cnp->cn_nameptr, cnp->cn_namelen,
	    &parms.Info, &vp);
	if (error != 0) {
		(void)vboxvfs_host_close(vmp, parms.Handle);
		return (error);
	}
	/* Keep the handle; the open that follows wants to write. */
	np = VTOVBOXVFS(vp);
	VBOXVFS_NODE_LOCK(np);
	if (np->vn_handle == SHFL_HANDLE_NIL) {
		np->vn_handle = parms.Handle;
		np->vn_haccess = SHFL_CF_ACCESS_READWRITE;
		parms.Handle = SHFL_HANDLE_NIL;
	}
	VBOXVFS_NODE_UNLOCK(np);
	if (parms.Handle != SHFL_HANDLE_NIL)
		(void)vboxvfs_host_close(vmp, parms.Handle);
	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(dvp, vp, cnp);
	*ap->a_vpp = vp;
	return (0);
}

static int
vboxvfs_mkdir(struct vop_mkdir_args *ap)
{
	struct componentname *cnp;
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *dnp;
	struct vnode *dvp, *vp;
	SHFLCREATEPARMS parms;
	char *path;
	size_t pathlen;
	mode_t mode;
	int error;

	dvp = ap->a_dvp;
	cnp = ap->a_cnp;
	dnp = VTOVBOXVFS(dvp);
	vmp = dnp->vn_vmp;
	mode = ap->a_vap->va_mode & ALLPERMS;
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "mkdir \"%.*s\" mode %#o in %ju\n",
	    (int)cnp->cn_namelen, cnp->cn_nameptr, mode, (uintmax_t)dnp->vn_ino);

	error = vboxvfs_node_childpath(dnp, cnp->cn_nameptr, cnp->cn_namelen,
	    &path, &pathlen);
	if (error != 0)
		return (error);
	error = vboxvfs_host_create(vmp, path, pathlen,
	    SHFL_CF_DIRECTORY | SHFL_CF_ACT_CREATE_IF_NEW |
	    SHFL_CF_ACT_FAIL_IF_EXISTS | SHFL_CF_ACCESS_READ |
	    SHFL_CF_ACCESS_ATTR_WRITE | SHFL_CF_ACCESS_DENYNONE, mode, &parms);
	vboxvfs_node_freepath(path);
	if (error != 0)
		return (error);
	if (parms.Handle == SHFL_HANDLE_NIL)
		return (vboxvfs_create_result(parms.Result));
	vboxvfs_node_invalattr(dnp);
	/* The host ignores the mode of new directories (0777 & umask). */
	if ((parms.Info.Attr.fMode & ALLPERMS) != mode)
		vboxvfs_fixmode(vmp, parms.Handle, RTFS_TYPE_DIRECTORY, mode,
		    &parms.Info);
	(void)vboxvfs_host_close(vmp, parms.Handle);

	error = vboxvfs_node_get(vmp, dvp, cnp->cn_nameptr, cnp->cn_namelen,
	    &parms.Info, &vp);
	if (error != 0)
		return (error);
	if (cnp->cn_flags & MAKEENTRY)
		cache_enter(dvp, vp, cnp);
	*ap->a_vpp = vp;
	return (0);
}

static int
vboxvfs_remove(struct vop_remove_args *ap)
{
	struct vboxvfs_node *dnp, *np;
	struct vnode *dvp, *vp;
	char *path;
	size_t pathlen;
	int error;

	dvp = ap->a_dvp;
	vp = ap->a_vp;
	dnp = VTOVBOXVFS(dvp);
	np = VTOVBOXVFS(vp);
	if (vp->v_type == VDIR)
		return (EPERM);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "remove ino %ju from %ju\n",
	    (uintmax_t)np->vn_ino, (uintmax_t)dnp->vn_ino);

	error = vboxvfs_node_path(np, &path, &pathlen);
	if (error != 0)
		return (error);
	error = vboxvfs_host_remove(np->vn_vmp, path, pathlen,
	    vp->v_type == VLNK ? SHFL_REMOVE_SYMLINK : SHFL_REMOVE_FILE);
	vboxvfs_node_freepath(path);
	if (error != 0)
		return (error);
	/*
	 * XXX A Windows host refuses to remove an open file (sharing
	 * violation, EBUSY); SHFL_FN_CLOSE_AND_REMOVE is the way out there.
	 */
	vboxvfs_node_remove(np);
	vboxvfs_node_invalattr(dnp);
	return (0);
}

static int
vboxvfs_rmdir(struct vop_rmdir_args *ap)
{
	struct vboxvfs_node *dnp, *np;
	struct vnode *dvp, *vp;
	char *path;
	size_t pathlen;
	int error;

	dvp = ap->a_dvp;
	vp = ap->a_vp;
	dnp = VTOVBOXVFS(dvp);
	np = VTOVBOXVFS(vp);
	if (dvp == vp)
		return (EINVAL);
	if (vp->v_vflag & VV_ROOT)
		return (EBUSY);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "rmdir ino %ju from %ju\n",
	    (uintmax_t)np->vn_ino, (uintmax_t)dnp->vn_ino);

	error = vboxvfs_node_path(np, &path, &pathlen);
	if (error != 0)
		return (error);
	error = vboxvfs_host_remove(np->vn_vmp, path, pathlen, SHFL_REMOVE_DIR);
	vboxvfs_node_freepath(path);
	if (error != 0)
		return (error);
	sx_xlock(&np->vn_dirsx);
	vboxvfs_node_freedir(np);
	sx_xunlock(&np->vn_dirsx);
	vn_seqc_write_begin(dvp);
	vn_seqc_write_begin(vp);
	vboxvfs_node_remove(np);
	cache_vop_rmdir(dvp, vp);
	vn_seqc_write_end(vp);
	vn_seqc_write_end(dvp);
	vboxvfs_node_invalattr(dnp);
	return (0);
}

/*
 * Rename.  Per the VOP_RENAME protocol tdvp and tvp come in locked, fdvp
 * and fvp only referenced, and all four must be released here.  The host
 * does the work by path; afterwards the node of fvp is moved under its
 * new parent and name, which keeps the paths of everything below it
 * right without touching those nodes.
 */
static int
vboxvfs_rename(struct vop_rename_args *ap)
{
	struct componentname *fcnp, *tcnp;
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *fdnp, *fnp, *tdnp, *tnp;
	struct vnode *fdvp, *fvp, *tdvp, *tvp;
	char *src, *dst;
	size_t srclen, dstlen;
	uint32_t rflags;
	int error;
	bool fvp_locked, seqc;

	fdvp = ap->a_fdvp;
	fvp = ap->a_fvp;
	fcnp = ap->a_fcnp;
	tdvp = ap->a_tdvp;
	tvp = ap->a_tvp;
	tcnp = ap->a_tcnp;
	src = dst = NULL;
	fvp_locked = false;
	seqc = false;

	if (fvp->v_mount != tdvp->v_mount ||
	    (tvp != NULL && fvp->v_mount != tvp->v_mount)) {
		error = EXDEV;
		goto out;
	}
	if (fvp == tvp) {
		error = 0;
		goto out;
	}
	if (fvp == tdvp) {
		/* Renaming a directory into itself. */
		error = EINVAL;
		goto out;
	}

	/*
	 * vfs_hash_rehash() wants fvp locked exclusively.  tdvp and tvp are
	 * already locked; try fvp without sleeping first, otherwise drop the
	 * target locks and take all three again, never sleeping on a target
	 * while holding fvp (a lookup may hold the directory and wait for
	 * fvp).
	 */
	if (vn_lock(fvp, LK_EXCLUSIVE | LK_NOWAIT) != 0) {
		VOP_UNLOCK(tdvp);
		if (tvp != NULL && tvp != tdvp)
			VOP_UNLOCK(tvp);
		for (;;) {
			vn_lock(fvp, LK_EXCLUSIVE | LK_RETRY);
			if (vn_lock(tdvp, LK_EXCLUSIVE | LK_NOWAIT) != 0) {
				VOP_UNLOCK(fvp);
				vn_lock(tdvp, LK_EXCLUSIVE | LK_RETRY);
				VOP_UNLOCK(tdvp);
				continue;
			}
			if (tvp != NULL && tvp != tdvp &&
			    vn_lock(tvp, LK_EXCLUSIVE | LK_NOWAIT) != 0) {
				VOP_UNLOCK(tdvp);
				VOP_UNLOCK(fvp);
				vn_lock(tvp, LK_EXCLUSIVE | LK_RETRY);
				VOP_UNLOCK(tvp);
				continue;
			}
			break;
		}
		if (VN_IS_DOOMED(fvp) || VN_IS_DOOMED(tdvp) ||
		    (tvp != NULL && VN_IS_DOOMED(tvp))) {
			VOP_UNLOCK(fvp);
			error = ENOENT;
			goto out;
		}
	}
	fvp_locked = true;
	/* Tell lockless lookups that these vnodes are being modified. */
	if (tvp != NULL)
		vn_seqc_write_begin(tvp);
	vn_seqc_write_begin(tdvp);
	vn_seqc_write_begin(fvp);
	vn_seqc_write_begin(fdvp);
	seqc = true;
	fdnp = VTOVBOXVFS(fdvp);
	fnp = VTOVBOXVFS(fvp);
	tdnp = VTOVBOXVFS(tdvp);
	tnp = tvp != NULL ? VTOVBOXVFS(tvp) : NULL;
	vmp = fnp->vn_vmp;
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP,
	    "rename \"%.*s\" (ino %ju) in %ju -> \"%.*s\" in %ju%s\n",
	    (int)fcnp->cn_namelen, fcnp->cn_nameptr, (uintmax_t)fnp->vn_ino,
	    (uintmax_t)fdnp->vn_ino, (int)tcnp->cn_namelen, tcnp->cn_nameptr,
	    (uintmax_t)tdnp->vn_ino, tvp != NULL ? " (replacing)" : "");

	if ((fvp->v_vflag & VV_ROOT) != 0 ||
	    (tvp != NULL && (tvp->v_vflag & VV_ROOT) != 0)) {
		error = EINVAL;
		goto out;
	}
	if (tvp != NULL) {
		if (fvp->v_type == VDIR && tvp->v_type != VDIR) {
			error = ENOTDIR;
			goto out;
		}
		if (fvp->v_type != VDIR && tvp->v_type == VDIR) {
			error = EISDIR;
			goto out;
		}
	}
	/* A directory cannot be moved into its own subtree. */
	if (fvp->v_type == VDIR && vboxvfs_node_isunder(tdnp, fnp)) {
		error = EINVAL;
		goto out;
	}

	error = vboxvfs_node_childpath(fdnp, fcnp->cn_nameptr, fcnp->cn_namelen,
	    &src, &srclen);
	if (error != 0)
		goto out;
	error = vboxvfs_node_childpath(tdnp, tcnp->cn_nameptr, tcnp->cn_namelen,
	    &dst, &dstlen);
	if (error != 0)
		goto out;
	rflags = fvp->v_type == VDIR ? SHFL_RENAME_DIR : SHFL_RENAME_FILE;
	if (tvp != NULL)
		rflags |= SHFL_RENAME_REPLACE_IF_EXISTS;
	error = vboxvfs_host_rename(vmp, src, srclen, dst, dstlen, rflags);
	if (error != 0)
		goto out;

	if (tvp != NULL) {
		if (tvp->v_type == VDIR) {
			sx_xlock(&tnp->vn_dirsx);
			vboxvfs_node_freedir(tnp);
			sx_xunlock(&tnp->vn_dirsx);
		}
		vboxvfs_node_remove(tnp);
	}
	vboxvfs_node_rename(fnp, tdvp, tcnp->cn_nameptr, tcnp->cn_namelen);
	cache_vop_rename(fdvp, fvp, tdvp, tvp, fcnp, tcnp);
	vboxvfs_node_invalattr(fdnp);
	vboxvfs_node_invalattr(tdnp);
	vboxvfs_node_invalattr(fnp);
out:
	if (seqc) {
		if (tvp != NULL)
			vn_seqc_write_end(tvp);
		vn_seqc_write_end(tdvp);
		vn_seqc_write_end(fvp);
		vn_seqc_write_end(fdvp);
	}
	if (src != NULL)
		vboxvfs_node_freepath(src);
	if (dst != NULL)
		vboxvfs_node_freepath(dst);
	if (tdvp == tvp)
		vrele(tdvp);
	else
		vput(tdvp);
	if (tvp != NULL)
		vput(tvp);
	vrele(fdvp);
	if (fvp_locked)
		vput(fvp);
	else
		vrele(fvp);
	return (error);
}

static int
vboxvfs_symlink(struct vop_symlink_args *ap)
{
	struct componentname *cnp;
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *dnp;
	struct vnode *dvp, *vp;
	SHFLFSOBJINFO info;
	char *path;
	size_t pathlen;
	int error;

	dvp = ap->a_dvp;
	cnp = ap->a_cnp;
	dnp = VTOVBOXVFS(dvp);
	vmp = dnp->vn_vmp;
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "symlink \"%.*s\" -> \"%s\" in %ju\n",
	    (int)cnp->cn_namelen, cnp->cn_nameptr, ap->a_target,
	    (uintmax_t)dnp->vn_ino);

	error = vboxvfs_node_childpath(dnp, cnp->cn_nameptr, cnp->cn_namelen,
	    &path, &pathlen);
	if (error != 0)
		return (error);
	error = vboxvfs_host_symlink(vmp, path, pathlen, ap->a_target,
	    strlen(ap->a_target), &info);
	vboxvfs_node_freepath(path);
	if (error != 0) {
		/*
		 * Symbolic link creation is disabled per share by default
		 * (VBoxInternal2/SharedFoldersEnableSymlinksCreate/<share>).
		 */
		if (error == EOPNOTSUPP ||
		    (error == EROFS && (dvp->v_mount->mnt_flag & MNT_RDONLY) == 0)) {
			VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR, "symlink creation "
			    "refused by the host; is SharedFoldersEnableSymlinksCreate "
			    "set for share \"%s\"?\n", vmp->vm_share);
			error = EPERM;
		}
		return (error);
	}
	vboxvfs_node_invalattr(dnp);
	error = vboxvfs_node_get(vmp, dvp, cnp->cn_nameptr, cnp->cn_namelen,
	    &info, &vp);
	if (error != 0)
		return (error);
	*ap->a_vpp = vp;
	return (0);
}

static int
vboxvfs_readlink(struct vop_readlink_args *ap)
{
	struct vboxvfs_node *np;
	struct vnode *vp;
	char *path, *buf;
	size_t pathlen, len;
	int error;

	vp = ap->a_vp;
	np = VTOVBOXVFS(vp);
	if (vp->v_type != VLNK)
		return (EINVAL);
	error = vboxvfs_node_path(np, &path, &pathlen);
	if (error != 0)
		return (error);
	buf = malloc(MAXPATHLEN, M_VBOXVFS, M_WAITOK);
	error = vboxvfs_host_readlink(np->vn_vmp, path, pathlen, buf, MAXPATHLEN,
	    &len);
	vboxvfs_node_freepath(path);
	if (error == 0)
		error = uiomove(buf, len, ap->a_uio);
	free(buf, M_VBOXVFS);
	return (error);
}

static int
vboxvfs_setattr(struct vop_setattr_args *ap)
{
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *np;
	struct vnode *vp;
	struct vattr *vap;
	struct ucred *cred;
	SHFLFSOBJINFO info;
	SHFLHANDLE handle;
	uint64_t oldsize;
	int error;
	bool rdonly;

	vp = ap->a_vp;
	vap = ap->a_vap;
	cred = ap->a_cred;
	np = VTOVBOXVFS(vp);
	vmp = np->vn_vmp;
	rdonly = (vp->v_mount->mnt_flag & MNT_RDONLY) != 0;

	if (vap->va_flags != VNOVAL)
		return (EOPNOTSUPP);
	if (vap->va_type != VNON || vap->va_nlink != VNOVAL ||
	    vap->va_fsid != VNOVAL || vap->va_fileid != VNOVAL ||
	    vap->va_blocksize != VNOVAL || vap->va_gen != VNOVAL ||
	    vap->va_rdev != VNOVAL || vap->va_bytes != VNOVAL)
		return (EINVAL);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_VOP, "setattr ino %ju mode %#o size %jd "
	    "uid %d gid %d\n", (uintmax_t)np->vn_ino, vap->va_mode,
	    (intmax_t)vap->va_size, (int)vap->va_uid, (int)vap->va_gid);

	if (vap->va_uid != (uid_t)VNOVAL || vap->va_gid != (gid_t)VNOVAL) {
		if (rdonly)
			return (EROFS);
		/* The host has no owners; only the mount's identity exists. */
		if ((vap->va_uid != (uid_t)VNOVAL && vap->va_uid != vmp->vm_uid) ||
		    (vap->va_gid != (gid_t)VNOVAL && vap->va_gid != vmp->vm_gid))
			return (EPERM);
		error = VOP_ACCESS(vp, VADMIN, cred, curthread);
		if (error != 0)
			return (error);
	}

	if (vap->va_size != (u_quad_t)VNOVAL) {
		switch (vp->v_type) {
		case VDIR:
			return (EISDIR);
		case VREG:
			break;
		case VLNK:
		default:
			return (EINVAL);
		}
		if (rdonly)
			return (EROFS);
		error = vboxvfs_node_handle(np, SHFL_CF_ACCESS_WRITE, &handle);
		if (error != 0)
			return (error);
		error = vboxvfs_host_setsize(vmp, handle, vap->va_size);
		if (error != 0)
			return (error);
		VBOXVFS_NODE_LOCK(np);
		oldsize = np->vn_size;
		np->vn_size = vap->va_size;
		np->vn_flags |= VBOXVFS_NODE_MODIFIED;
		VBOXVFS_NODE_UNLOCK(np);
		if (vap->va_size < oldsize)
			(void)vtruncbuf(vp, vap->va_size, vmp->vm_biosize);
		vnode_pager_setsize(vp, vap->va_size);
		vboxvfs_node_invalattr(np);
	}

	if (vap->va_mode != (mode_t)VNOVAL) {
		if (rdonly)
			return (EROFS);
		error = VOP_ACCESS(vp, VADMIN, cred, curthread);
		if (error != 0)
			return (error);
		error = vboxvfs_node_handle(np, SHFL_CF_ACCESS_ATTR_WRITE,
		    &handle);
		if (error != 0)
			return (error);
		RT_ZERO(info);
		info.Attr.fMode = vboxvfs_type2rtfs(vp->v_type) |
		    (vap->va_mode & ALLPERMS);
		info.Attr.enmAdditional = SHFLFSOBJATTRADD_NOTHING;
		error = vboxvfs_host_setinfo(vmp, handle, &info);
		if (error != 0)
			return (error);
		vboxvfs_node_invalattr(np);
	}

	if (vap->va_atime.tv_sec != VNOVAL || vap->va_mtime.tv_sec != VNOVAL ||
	    vap->va_birthtime.tv_sec != VNOVAL) {
		if (rdonly)
			return (EROFS);
		error = VOP_ACCESS(vp, VADMIN, cred, curthread);
		if (error != 0) {
			if ((vap->va_vaflags & VA_UTIMES_NULL) == 0)
				return (error);
			error = VOP_ACCESS(vp, VWRITE, cred, curthread);
			if (error != 0)
				return (error);
		}
		error = vboxvfs_node_handle(np, SHFL_CF_ACCESS_ATTR_WRITE,
		    &handle);
		if (error != 0)
			return (error);
		/*
		 * Zero means "leave alone" to the host, so the epoch itself
		 * cannot be set; use the next nanosecond instead.
		 */
		RT_ZERO(info);
		info.Attr.enmAdditional = SHFLFSOBJATTRADD_NOTHING;
		if (vap->va_atime.tv_sec != VNOVAL) {
			info.AccessTime.i64NanosecondsRelativeToUnixEpoch =
			    vboxvfs_ts2ns(&vap->va_atime);
			if (info.AccessTime.i64NanosecondsRelativeToUnixEpoch == 0)
				info.AccessTime.i64NanosecondsRelativeToUnixEpoch = 1;
		}
		if (vap->va_mtime.tv_sec != VNOVAL) {
			info.ModificationTime.i64NanosecondsRelativeToUnixEpoch =
			    vboxvfs_ts2ns(&vap->va_mtime);
			if (info.ModificationTime.i64NanosecondsRelativeToUnixEpoch == 0)
				info.ModificationTime.i64NanosecondsRelativeToUnixEpoch = 1;
		}
		if (vap->va_birthtime.tv_sec != VNOVAL) {
			info.BirthTime.i64NanosecondsRelativeToUnixEpoch =
			    vboxvfs_ts2ns(&vap->va_birthtime);
			if (info.BirthTime.i64NanosecondsRelativeToUnixEpoch == 0)
				info.BirthTime.i64NanosecondsRelativeToUnixEpoch = 1;
		}
		error = vboxvfs_host_setinfo(vmp, handle, &info);
		if (error != 0)
			return (error);
		vboxvfs_node_invalattr(np);
	}
	return (0);
}
