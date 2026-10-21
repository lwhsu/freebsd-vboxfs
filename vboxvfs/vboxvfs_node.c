/*-
 * SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
 *
 * Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
 */

/*
 * vboxvfs -- node cache: one node per vnode, found through vfs_hash by
 * (parent node, name); paths are rebuilt from the chain of parents when a
 * host request needs one, so renaming a directory keeps its descendants
 * correct without touching them.
 */

#include "vboxvfs_internal.h"

#include <sys/fnv_hash.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/sx.h>

extern struct sx vboxvfs_rename_lock;

struct vboxvfs_cmparg {
	struct vboxvfs_node	*parent;
	const char		*name;
	u_int			 namelen;
};

static int
vboxvfs_vnode_cmp(struct vnode *vp, void *arg)
{
	struct vboxvfs_node *np;
	struct vboxvfs_cmparg *ca;

	np = VTOVBOXVFS(vp);
	ca = arg;
	if (np->vn_parent != ca->parent || np->vn_namelen != ca->namelen ||
	    bcmp(np->vn_name, ca->name, ca->namelen) != 0)
		return (1);
	return (0);
}

/*
 * vfs_hash key: the parent node pointer and the name.
 */
u_int
vboxvfs_node_hash(const struct vboxvfs_node *dnp, const char *name,
    u_int namelen)
{
	Fnv32_t h;

	h = fnv_32_buf(&dnp, sizeof(dnp), FNV1_32_INIT);
	return (fnv_32_buf(name, namelen, h));
}

/*
 * Inode numbers are a 64-bit FNV-1a hash of the path relative to the share
 * root, computed once when the node is created (and so stable across
 * renames while the node lives).  The root is VBOXVFS_ROOT_INO.
 */
static ino_t
vboxvfs_ino_finish(Fnv64_t h)
{

	if (h <= VBOXVFS_ROOT_INO)
		h += VBOXVFS_ROOT_INO + 1;
	return ((ino_t)h);
}

ino_t
vboxvfs_node_ino(const char *path, size_t len)
{

	if (len == 0)
		return (VBOXVFS_ROOT_INO);
	return (vboxvfs_ino_finish(fnv_64_buf(path, len, FNV1_64_INIT)));
}

/*
 * Same as vboxvfs_node_ino() for "<dirpath>/<name>" without building the
 * string; used by readdir for entries that have no node.
 */
ino_t
vboxvfs_node_childino(const char *dirpath, size_t dirlen, const char *name,
    u_int namelen)
{
	Fnv64_t h;

	h = FNV1_64_INIT;
	if (dirlen != 0) {
		h = fnv_64_buf(dirpath, dirlen, h);
		h = fnv_64_buf("/", 1, h);
	}
	return (vboxvfs_ino_finish(fnv_64_buf(name, namelen, h)));
}

/*
 * Build the host path of a node, optionally extended by one more
 * component.  The result has no leading or trailing slash, the root is the
 * empty string.  Free with vboxvfs_node_freepath().
 */
static int
vboxvfs_build_path(struct vboxvfs_node *np, const char *name, u_int namelen,
    char **pathp, size_t *lenp)
{
	struct vboxvfs_node *p;
	char *buf;
	size_t len, pos;

	sx_slock(&vboxvfs_rename_lock);
	len = namelen;
	for (p = np; (p->vn_flags & VBOXVFS_NODE_ROOT) == 0; p = p->vn_parent) {
		if (len != 0)
			len++;			/* separator */
		len += p->vn_namelen;
	}
	if (len >= MAXPATHLEN) {
		sx_sunlock(&vboxvfs_rename_lock);
		return (ENAMETOOLONG);
	}
	buf = malloc(len + 1, M_VBOXVFS, M_WAITOK);
	buf[len] = '\0';
	pos = len;
	if (namelen != 0) {
		pos -= namelen;
		bcopy(name, buf + pos, namelen);
		if (pos != 0)
			buf[--pos] = '/';
	}
	for (p = np; (p->vn_flags & VBOXVFS_NODE_ROOT) == 0; p = p->vn_parent) {
		pos -= p->vn_namelen;
		bcopy(p->vn_name, buf + pos, p->vn_namelen);
		if (pos != 0)
			buf[--pos] = '/';
	}
	sx_sunlock(&vboxvfs_rename_lock);
	KASSERT(pos == 0, ("vboxvfs_build_path: pos %zu", pos));
	*pathp = buf;
	*lenp = len;
	return (0);
}

int
vboxvfs_node_path(struct vboxvfs_node *np, char **pathp, size_t *lenp)
{

	return (vboxvfs_build_path(np, NULL, 0, pathp, lenp));
}

int
vboxvfs_node_childpath(struct vboxvfs_node *dnp, const char *name,
    u_int namelen, char **pathp, size_t *lenp)
{

	return (vboxvfs_build_path(dnp, name, namelen, pathp, lenp));
}

void
vboxvfs_node_freepath(char *path)
{

	free(path, M_VBOXVFS);
}

__enum_uint8(vtype)
vboxvfs_mode2type(RTFMODE fmode)
{

	switch (fmode & RTFS_TYPE_MASK) {
	case RTFS_TYPE_DIRECTORY:
		return (VDIR);
	case RTFS_TYPE_SYMLINK:
		return (VLNK);
	case RTFS_TYPE_FIFO:
		return (VFIFO);
	case RTFS_TYPE_SOCKET:
		return (VSOCK);
	case RTFS_TYPE_DEV_CHAR:
		return (VCHR);
	case RTFS_TYPE_DEV_BLOCK:
		return (VBLK);
	case RTFS_TYPE_FILE:
	default:
		return (VREG);
	}
}

/*
 * The permission bits shown for an object: the host's unless the mount
 * fixes them, minus the mask.
 */
mode_t
vboxvfs_mode(struct vboxvfs_mount *vmp, __enum_uint8(vtype) type,
    RTFMODE fmode)
{
	mode_t mode;

	mode = fmode & ALLPERMS;
	if (type == VDIR) {
		if (vmp->vm_dmode != 0)
			mode = vmp->vm_dmode;
		mode &= ~vmp->vm_dmask;
	} else {
		if (vmp->vm_fmode != 0)
			mode = vmp->vm_fmode;
		mode &= ~vmp->vm_fmask;
	}
	return (mode);
}

/*
 * Record attributes reported by the host.
 */
void
vboxvfs_node_setinfo(struct vboxvfs_node *np, const SHFLFSOBJINFO *info)
{
	sbintime_t now;

	now = sbinuptime();
	if (now == 0)
		now = 1;
	VBOXVFS_NODE_LOCK(np);
	np->vn_info = *info;
	np->vn_info_time = now;
	np->vn_size = info->cbObject;
	np->vn_mtime_ns =
	    info->ModificationTime.i64NanosecondsRelativeToUnixEpoch;
	VBOXVFS_NODE_UNLOCK(np);
}

void
vboxvfs_node_invalattr(struct vboxvfs_node *np)
{

	VBOXVFS_NODE_LOCK(np);
	np->vn_info_time = 0;
	VBOXVFS_NODE_UNLOCK(np);
}

/*
 * Make sure the cached attributes are fresh (younger than the ttl, or
 * re-fetched when force is set).  Detects objects that vanished (ENOENT,
 * node marked stale) or changed type (ESTALE), and purges the name cache
 * of a directory whose modification time changed.  Called with the vnode
 * locked, shared or exclusive.
 */
int
vboxvfs_node_revalidate(struct vboxvfs_node *np, bool force)
{
	struct vboxvfs_mount *vmp;
	struct vnode *vp;
	SHFLFSOBJINFO info;
	SHFLHANDLE handle;
	char *path;
	size_t pathlen;
	uint64_t oldsize;
	int64_t oldmtime;
	int error;
	bool changed;

	vp = VBOXVFSTOV(np);
	vmp = np->vn_vmp;

	VBOXVFS_NODE_LOCK(np);
	if (!force && np->vn_info_time != 0 &&
	    sbinuptime() - np->vn_info_time < vmp->vm_ttl) {
		VBOXVFS_NODE_UNLOCK(np);
		return (0);
	}
	handle = np->vn_handle;
	VBOXVFS_NODE_UNLOCK(np);
	if (np->vn_flags & VBOXVFS_NODE_ROOT)
		handle = vmp->vm_roothandle;

	if (handle != SHFL_HANDLE_NIL) {
		error = vboxvfs_host_getinfo(vmp, handle, &info);
	} else {
		error = vboxvfs_node_path(np, &path, &pathlen);
		if (error != 0)
			return (error);
		error = vboxvfs_host_lookup(vmp, path, pathlen, &info);
		vboxvfs_node_freepath(path);
	}
	if (error == ENOENT) {
		VBOXVFS_NODE_LOCK(np);
		np->vn_flags |= VBOXVFS_NODE_STALE;
		VBOXVFS_NODE_UNLOCK(np);
		cache_purge(vp);
		return (ENOENT);
	}
	if (error != 0)
		return (error);
	if (vboxvfs_mode2type(info.Attr.fMode) != vp->v_type) {
		VBOXVFS_NODE_LOCK(np);
		np->vn_flags |= VBOXVFS_NODE_STALE;
		VBOXVFS_NODE_UNLOCK(np);
		cache_purge(vp);
		return (ESTALE);
	}

	VBOXVFS_NODE_LOCK(np);
	oldsize = np->vn_size;
	oldmtime = np->vn_mtime_ns;
	np->vn_info = info;
	np->vn_info_time = sbinuptime();
	if (np->vn_info_time == 0)
		np->vn_info_time = 1;
	np->vn_size = info.cbObject;
	np->vn_mtime_ns = info.ModificationTime.i64NanosecondsRelativeToUnixEpoch;
	changed = oldsize != np->vn_size || oldmtime != np->vn_mtime_ns;
	VBOXVFS_NODE_UNLOCK(np);

	if (changed && vp->v_type == VDIR)
		cache_purge(vp);
	/* XXX Phase 3: VREG changed -> vinvalbuf + vnode_pager_setsize. */
	return (0);
}

/*
 * Open the directory of a node on the host, read-only, for listing.
 */
int
vboxvfs_node_opendir(struct vboxvfs_node *np, SHFLHANDLE *handlep)
{
	SHFLCREATEPARMS parms;
	char *path;
	size_t pathlen;
	int error;

	error = vboxvfs_node_path(np, &path, &pathlen);
	if (error != 0)
		return (error);
	error = vboxvfs_host_create(np->vn_vmp, path, pathlen,
	    VBOXVFS_CREATE_DIR_RO, 0, &parms);
	vboxvfs_node_freepath(path);
	if (error != 0)
		return (error);
	if (parms.Handle == SHFL_HANDLE_NIL) {
		switch (parms.Result) {
		case SHFL_FILE_NOT_FOUND:
		case SHFL_PATH_NOT_FOUND:
			VBOXVFS_NODE_LOCK(np);
			np->vn_flags |= VBOXVFS_NODE_STALE;
			VBOXVFS_NODE_UNLOCK(np);
			return (ENOENT);
		default:
			return (EIO);
		}
	}
	vboxvfs_node_setinfo(np, &parms.Info);
	*handlep = parms.Handle;
	return (0);
}

/*
 * Close the node's host handle, if any.
 */
void
vboxvfs_node_closehandle(struct vboxvfs_node *np)
{
	SHFLHANDLE handle;

	VBOXVFS_NODE_LOCK(np);
	handle = np->vn_handle;
	np->vn_handle = SHFL_HANDLE_NIL;
	np->vn_haccess = 0;
	VBOXVFS_NODE_UNLOCK(np);
	if (handle != SHFL_HANDLE_NIL)
		(void)vboxvfs_host_close(np->vn_vmp, handle);
}

/*
 * Drop the cached directory listing.  Called with vn_dirsx held.
 */
void
vboxvfs_node_freedir(struct vboxvfs_node *np)
{

	free(np->vn_dirbuf, M_VBOXVFS);
	free(np->vn_diroffs, M_VBOXVFS);
	np->vn_dirbuf = NULL;
	np->vn_diroffs = NULL;
	np->vn_dirbuflen = 0;
	np->vn_dirbufsize = 0;
	np->vn_dircnt = 0;
	np->vn_diroffcnt = 0;
}

/*
 * Free a node that is no longer attached to a vnode.
 */
void
vboxvfs_node_free(struct vboxvfs_node *np)
{

	KASSERT(np->vn_handle == SHFL_HANDLE_NIL,
	    ("vboxvfs_node_free: host handle still open"));
	if (np->vn_flags & VBOXVFS_NODE_REFPARENT)
		vrele(VBOXVFSTOV(np->vn_parent));
	vboxvfs_node_freedir(np);
	free(np->vn_name, M_VBOXVFS);
	sx_destroy(&np->vn_dirsx);
	mtx_destroy(&np->vn_mtx);
	free(np, M_VBOXVFS);
}

/*
 * Find or create the node for <dvp>/<name>.  dvp is NULL for the root.
 * info must be given to create a node; without it only an existing one is
 * returned (ENOENT otherwise).  The vnode is returned locked exclusively
 * and referenced.  dvp must be locked.
 */
int
vboxvfs_node_get(struct vboxvfs_mount *vmp, struct vnode *dvp,
    const char *name, u_int namelen, const SHFLFSOBJINFO *info,
    struct vnode **vpp)
{
	struct mount *mp;
	struct thread *td;
	struct vboxvfs_cmparg ca;
	struct vboxvfs_node *np, *dnp;
	struct vnode *vp, *vp2;
	char *path;
	size_t pathlen;
	ino_t ino;
	u_int hash;
	int error;

	mp = vmp->vm_mp;
	td = curthread;
	dnp = dvp != NULL ? VTOVBOXVFS(dvp) : NULL;
	ca.parent = dnp;
	ca.name = name;
	ca.namelen = namelen;
	hash = vboxvfs_node_hash(dnp, name, namelen);

	error = vfs_hash_get(mp, hash, LK_EXCLUSIVE, td, vpp, vboxvfs_vnode_cmp,
	    &ca);
	if (error != 0)
		return (error);
	if (*vpp != NULL) {
		np = VTOVBOXVFS(*vpp);
		if (info == NULL)
			return (0);
		if (vboxvfs_mode2type(info->Attr.fMode) == (*vpp)->v_type) {
			vboxvfs_node_setinfo(np, info);
			return (0);
		}
		/*
		 * Something of a different type took the name on the host.
		 * Kill the stale vnode and make a new one below.
		 */
		VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR,
		    "type of \"%.*s\" changed on the host, dropping vnode\n",
		    (int)namelen, name);
		vgone(*vpp);
		vput(*vpp);
		*vpp = NULL;
	}
	if (info == NULL)
		return (ENOENT);

	if (dnp != NULL) {
		error = vboxvfs_node_childpath(dnp, name, namelen, &path,
		    &pathlen);
		if (error != 0)
			return (error);
		ino = vboxvfs_node_ino(path, pathlen);
		vboxvfs_node_freepath(path);
	} else
		ino = VBOXVFS_ROOT_INO;

	error = getnewvnode(VBOXVFS_VFSNAME, mp, &vboxvfs_vnodeops, &vp);
	if (error != 0)
		return (error);
	np = malloc(sizeof(*np), M_VBOXVFS, M_WAITOK | M_ZERO);
	np->vn_name = malloc(namelen + 1, M_VBOXVFS, M_WAITOK);
	bcopy(name, np->vn_name, namelen);
	np->vn_name[namelen] = '\0';
	np->vn_namelen = namelen;
	np->vn_vp = vp;
	np->vn_vmp = vmp;
	np->vn_ino = ino;
	np->vn_handle = SHFL_HANDLE_NIL;
	mtx_init(&np->vn_mtx, "vboxvfs node", NULL, MTX_DEF);
	sx_init(&np->vn_dirsx, "vboxvfs dir");
	if (dnp == NULL) {
		np->vn_parent = np;
		np->vn_flags |= VBOXVFS_NODE_ROOT;
	} else {
		np->vn_parent = dnp;
		/*
		 * Children keep their parent alive.  The root is kept by the
		 * mount itself and must not be referenced, or vflush() could
		 * never retire it.
		 */
		if ((dnp->vn_flags & VBOXVFS_NODE_ROOT) == 0) {
			vref(dvp);
			np->vn_flags |= VBOXVFS_NODE_REFPARENT;
		}
	}
	vboxvfs_node_setinfo(np, info);

	lockmgr(vp->v_vnlock, LK_EXCLUSIVE, NULL);
	vp->v_type = vboxvfs_mode2type(info->Attr.fMode);
	vp->v_data = np;
	vp->v_bufobj.bo_bsize = vmp->vm_biosize;
	if (vp->v_type != VFIFO)
		VN_LOCK_ASHARE(vp);
	error = insmntque(vp, mp);
	if (error != 0) {
		/* insmntque() has disposed of the vnode. */
		vboxvfs_node_free(np);
		return (error);
	}
	vn_set_state(vp, VSTATE_CONSTRUCTED);
	error = vfs_hash_insert(vp, hash, LK_EXCLUSIVE, td, &vp2,
	    vboxvfs_vnode_cmp, &ca);
	if (error != 0)
		return (error);
	if (vp2 != NULL) {
		/* Lost a race; our vnode has been released already. */
		*vpp = vp2;
		return (0);
	}
	if (vp->v_type == VREG)
		vnode_create_vobject(vp, info->cbObject, td);
	*vpp = vp;
	return (0);
}

/*
 * Look up a cached node by (parent, name) without locking it; *vpp is
 * NULL or a referenced vnode.  Used by readdir for inode numbers.
 */
void
vboxvfs_node_ref(struct vboxvfs_node *dnp, const char *name, u_int namelen,
    struct vnode **vpp)
{
	struct vboxvfs_cmparg ca;

	ca.parent = dnp;
	ca.name = name;
	ca.namelen = namelen;
	vfs_hash_ref(dnp->vn_vmp->vm_mp, vboxvfs_node_hash(dnp, name, namelen),
	    curthread, vpp, vboxvfs_vnode_cmp, &ca);
}
