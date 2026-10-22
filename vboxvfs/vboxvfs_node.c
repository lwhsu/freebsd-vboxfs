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

#include <vm/vm.h>
#include <vm/vm_extern.h>
#include <vm/vm_object.h>

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
	    memcmp(np->vn_name, ca->name, ca->namelen) != 0)
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

/*
 * Record attributes reported by the host for a node that may have cached
 * data, and react to what changed since the attributes were last recorded:
 * a regular file whose size or modification time moved under us (and not
 * because of our own write-through, see VBOXVFS_NODE_MODIFIED) loses its
 * cached pages, a directory loses its name cache entries.  Every place that
 * learns fresh attributes of an existing node must come through here, or
 * the next revalidation compares against already updated values and the
 * stale data survives (a file replaced on the host was read as its old
 * self for as long as it stayed cached).  Called with the vnode locked,
 * shared or exclusive.
 */
void
vboxvfs_node_update(struct vboxvfs_node *np, const SHFLFSOBJINFO *info,
    const struct vboxvfs_issued *is)
{
	struct vnode *vp;
	sbintime_t now;
	uint64_t newsize, oldsize;
	int64_t oldmtime;
	bool changed, modified, trust;

	vp = VBOXVFSTOV(np);
	now = sbinuptime();
	if (now == 0)
		now = 1;
	VBOXVFS_NODE_LOCK(np);
	oldsize = np->vn_size;
	oldmtime = np->vn_mtime_ns;
	/*
	 * The answer is only meaningful for the size and for telling a
	 * foreign change from our own if none of our writes or size changes
	 * was in flight when the request was issued, started since, or is
	 * still in flight now.  A lookup answered with the pre-write size and
	 * applied once the write finished (it waits for the vnode lock the
	 * writer holds) used to shrink the VM object under a shared mapping,
	 * and the next access faulted with SIGBUS.
	 */
	trust = np->vn_inflight == 0 && (is == NULL ||
	    (!is->vi_inflight && np->vn_modtime < is->vi_time));
	modified = (np->vn_flags & VBOXVFS_NODE_MODIFIED) != 0;
	if (trust)
		np->vn_flags &= ~VBOXVFS_NODE_MODIFIED;
	np->vn_info = *info;
	np->vn_info_time = now;
	if (trust)
		np->vn_size = info->cbObject;
	else
		np->vn_info.cbObject = np->vn_size;
	newsize = np->vn_size;
	np->vn_mtime_ns = info->ModificationTime.i64NanosecondsRelativeToUnixEpoch;
	VBOXVFS_NODE_UNLOCK(np);
	if (!trust)
		return;
	changed = oldsize != newsize || oldmtime !=
	    info->ModificationTime.i64NanosecondsRelativeToUnixEpoch;
	if (!changed)
		return;
	if (vp->v_type == VDIR)
		cache_purge(vp);
	else if (vp->v_type == VREG) {
		if (!modified)
			(void)vinvalbuf(vp, V_SAVE, 0, 0);
		vboxvfs_node_setsize(vp, newsize);
	}
}

/*
 * Bracket a write or size change: host answers issued or completed while
 * one is in flight are not trusted for the size (see vboxvfs_node_update()).
 */
void
vboxvfs_node_modstart(struct vboxvfs_node *np)
{

	/*
	 * The mount-wide count goes up before the start time is recorded, and
	 * the snapshots in vboxvfs_{node,mount}_issue() take the time before
	 * they read the counts: a request that sees "nothing in flight" was
	 * then issued either before this modification started (its start time
	 * is later than the request) or after it ended.
	 */
	atomic_add_int(&np->vn_vmp->vm_inflight, 1);
	VBOXVFS_NODE_LOCK(np);
	np->vn_inflight++;
	np->vn_modtime = sbinuptime();
	VBOXVFS_NODE_UNLOCK(np);
}

void
vboxvfs_node_modend(struct vboxvfs_node *np)
{

	VBOXVFS_NODE_LOCK(np);
	np->vn_inflight--;
	VBOXVFS_NODE_UNLOCK(np);
	atomic_subtract_int(&np->vn_vmp->vm_inflight, 1);
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
	struct vboxvfs_issued is;
	SHFLHANDLE handle;
	char *path;
	size_t pathlen;
	int error;

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
	vboxvfs_node_issue(np, &is);

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

	vboxvfs_node_update(np, &info, &is);
	return (0);
}

/*
 * Open the directory of a node on the host, read-only, for listing.
 */
int
vboxvfs_node_opendir(struct vboxvfs_node *np, SHFLHANDLE *handlep)
{
	SHFLCREATEPARMS parms;
	struct vboxvfs_issued is;
	char *path;
	size_t pathlen;
	int error;

	error = vboxvfs_node_path(np, &path, &pathlen);
	if (error != 0)
		return (error);
	vboxvfs_node_issue(np, &is);
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
	vboxvfs_node_update(np, &parms.Info, &is);
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
    const struct vboxvfs_issued *is, struct vnode **vpp)
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
			vboxvfs_node_update(np, info, is);
			VBOXVFS_NODE_LOCK(np);
			np->vn_flags &= ~VBOXVFS_NODE_STALE;
			VBOXVFS_NODE_UNLOCK(np);
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

/*
 * Make sure the node has a host handle with at least the given access
 * (a combination of SHFL_CF_ACCESS_* bits).  Directories are opened as
 * such.  The handle stays with the node until it goes inactive.
 */
int
vboxvfs_node_handle(struct vboxvfs_node *np, uint32_t access,
    SHFLHANDLE *handlep)
{
	struct vboxvfs_mount *vmp;
	struct vnode *vp;
	SHFLCREATEPARMS parms;
	struct vboxvfs_issued is;
	SHFLHANDLE old;
	char *path;
	size_t pathlen;
	uint32_t cflags;
	int error;

	vp = VBOXVFSTOV(np);
	vmp = np->vn_vmp;

	VBOXVFS_NODE_LOCK(np);
	if (np->vn_handle != SHFL_HANDLE_NIL &&
	    (np->vn_haccess & access) == access) {
		*handlep = np->vn_handle;
		VBOXVFS_NODE_UNLOCK(np);
		return (0);
	}
	/* Keep what the existing handle has; the new one supersedes it. */
	access |= np->vn_haccess;
	VBOXVFS_NODE_UNLOCK(np);

	if (access & SHFL_CF_ACCESS_WRITE)
		access |= SHFL_CF_ACCESS_READ;
	cflags = SHFL_CF_ACT_OPEN_IF_EXISTS | SHFL_CF_ACT_FAIL_IF_NEW |
	    SHFL_CF_ACCESS_DENYNONE | access;
	if (vp->v_type == VDIR)
		cflags |= SHFL_CF_DIRECTORY;

	error = vboxvfs_node_path(np, &path, &pathlen);
	if (error != 0)
		return (error);
	vboxvfs_node_issue(np, &is);
	error = vboxvfs_host_create(vmp, path, pathlen, cflags, 0, &parms);
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
	vboxvfs_node_update(np, &parms.Info, &is);

	VBOXVFS_NODE_LOCK(np);
	if (np->vn_handle != SHFL_HANDLE_NIL &&
	    (np->vn_haccess & access) == access) {
		/* Somebody else opened a sufficient handle meanwhile. */
		old = parms.Handle;
		*handlep = np->vn_handle;
	} else {
		old = np->vn_handle;
		np->vn_handle = parms.Handle;
		np->vn_haccess = access;
		*handlep = parms.Handle;
	}
	VBOXVFS_NODE_UNLOCK(np);
	if (old != SHFL_HANDLE_NIL)
		(void)vboxvfs_host_close(vmp, old);
	return (0);
}

/*
 * A handle good enough for changing attributes: the node's own handle if it
 * can write (data or attributes), else a temporary one opened for attribute
 * writing only, which the caller must close when *tmpp is set.  Unlike
 * vboxvfs_node_handle() this never widens the node's handle: re-opening a
 * file that is open for writing once its new, read-only mode is in place
 * (cp -p, install(1)) is refused by the host, which is not root there.
 */
int
vboxvfs_node_attrhandle(struct vboxvfs_node *np, SHFLHANDLE *handlep,
    bool *tmpp)
{
	struct vboxvfs_mount *vmp;
	struct vnode *vp;
	SHFLCREATEPARMS parms;
	struct vboxvfs_issued is;
	char *path;
	size_t pathlen;
	uint32_t cflags;
	int error;

	vp = VBOXVFSTOV(np);
	vmp = np->vn_vmp;

	VBOXVFS_NODE_LOCK(np);
	if (np->vn_handle != SHFL_HANDLE_NIL && (np->vn_haccess &
	    (SHFL_CF_ACCESS_WRITE | SHFL_CF_ACCESS_ATTR_WRITE)) != 0) {
		*handlep = np->vn_handle;
		*tmpp = false;
		VBOXVFS_NODE_UNLOCK(np);
		return (0);
	}
	VBOXVFS_NODE_UNLOCK(np);

	cflags = SHFL_CF_ACT_OPEN_IF_EXISTS | SHFL_CF_ACT_FAIL_IF_NEW |
	    SHFL_CF_ACCESS_DENYNONE | SHFL_CF_ACCESS_ATTR_WRITE;
	if (vp->v_type == VDIR)
		cflags |= SHFL_CF_DIRECTORY;
	error = vboxvfs_node_path(np, &path, &pathlen);
	if (error != 0)
		return (error);
	vboxvfs_node_issue(np, &is);
	error = vboxvfs_host_create(vmp, path, pathlen, cflags, 0, &parms);
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
	vboxvfs_node_update(np, &parms.Info, &is);
	*handlep = parms.Handle;
	*tmpp = true;
	return (0);
}

/*
 * The object behind the node is gone (unlinked, or replaced by a rename):
 * make sure no lookup finds the node again and that it is recycled once
 * the last user is done with it.
 */
void
vboxvfs_node_remove(struct vboxvfs_node *np)
{
	struct vnode *vp;

	vp = VBOXVFSTOV(np);
	VBOXVFS_NODE_LOCK(np);
	np->vn_flags |= VBOXVFS_NODE_STALE;
	if ((np->vn_flags & VBOXVFS_NODE_UNHASHED) == 0) {
		np->vn_flags |= VBOXVFS_NODE_UNHASHED;
		VBOXVFS_NODE_UNLOCK(np);
		vfs_hash_remove(vp);
	} else
		VBOXVFS_NODE_UNLOCK(np);
	cache_purge(vp);
}

/*
 * Rename bookkeeping: move the node under a new parent and name and
 * rehash it.  Called after the host accepted the rename; the caller holds
 * the target directory's vnode lock.
 */
void
vboxvfs_node_rename(struct vboxvfs_node *np, struct vnode *tdvp,
    const char *name, u_int namelen)
{
	struct vboxvfs_node *tdnp, *oldparent;
	struct vnode *oldparentvp;
	char *newname;
	bool oldref;

	tdnp = VTOVBOXVFS(tdvp);
	newname = malloc(namelen + 1, M_VBOXVFS, M_WAITOK);
	bcopy(name, newname, namelen);
	newname[namelen] = '\0';

	sx_xlock(&vboxvfs_rename_lock);
	oldparent = np->vn_parent;
	oldref = (np->vn_flags & VBOXVFS_NODE_REFPARENT) != 0;
	free(np->vn_name, M_VBOXVFS);
	np->vn_name = newname;
	np->vn_namelen = namelen;
	np->vn_parent = tdnp;
	VBOXVFS_NODE_LOCK(np);
	np->vn_flags &= ~VBOXVFS_NODE_REFPARENT;
	if ((tdnp->vn_flags & VBOXVFS_NODE_ROOT) == 0) {
		vref(tdvp);
		np->vn_flags |= VBOXVFS_NODE_REFPARENT;
	}
	VBOXVFS_NODE_UNLOCK(np);
	if ((np->vn_flags & VBOXVFS_NODE_UNHASHED) == 0)
		vfs_hash_rehash(VBOXVFSTOV(np),
		    vboxvfs_node_hash(tdnp, name, namelen));
	sx_xunlock(&vboxvfs_rename_lock);

	oldparentvp = VBOXVFSTOV(oldparent);
	if (oldref)
		vrele(oldparentvp);
}

/*
 * Is "np" the same as, or a descendant of, "ancestor"?  Used to refuse
 * renaming a directory into its own subtree.
 */
bool
vboxvfs_node_isunder(struct vboxvfs_node *np, struct vboxvfs_node *ancestor)
{
	struct vboxvfs_node *p;
	bool found;

	found = false;
	sx_slock(&vboxvfs_rename_lock);
	for (p = np;; p = p->vn_parent) {
		if (p == ancestor) {
			found = true;
			break;
		}
		if (p->vn_flags & VBOXVFS_NODE_ROOT)
			break;
	}
	sx_sunlock(&vboxvfs_rename_lock);
	return (found);
}

/*
 * Tell the vnode pager about a new file size.  vnode_pager_setsize() wants
 * the vnode locked exclusively; from a shared-locked path (getattr during
 * mmap, for instance) leave a note and the VFS calls VOP_DELAYED_SETSIZE
 * when the vnode is next locked exclusively, as nfsclient does.
 */
void
vboxvfs_node_setsize(struct vnode *vp, uint64_t nsize)
{
	vm_object_t object;

	object = vp->v_object;
	if (object == NULL || nsize == object->un_pager.vnp.vnp_size)
		return;
	if (VOP_ISLOCKED(vp) == LK_EXCLUSIVE &&
	    (curthread->td_pflags2 & TDP2_SBPAGES) == 0)
		vnode_pager_setsize(vp, nsize);
	else
		vn_delayed_setsize(vp);
}
