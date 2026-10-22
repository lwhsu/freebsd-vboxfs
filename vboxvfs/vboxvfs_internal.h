/*-
 * SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
 *
 * Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
 */

/*
 * vboxvfs -- VirtualBox shared folder client for FreeBSD guests.
 *
 * Internal structures shared by the mount, node, vnode, I/O and host
 * request layers.  Everything here is kernel-only.
 */

#ifndef _VBOXVFS_INTERNAL_H_
#define	_VBOXVFS_INTERNAL_H_

#ifndef _KERNEL
#error "vboxvfs_internal.h is kernel only"
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mount.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/vnode.h>

/*
 * sys/priority.h defines PVM as a scheduling priority; VirtualBox uses the
 * name for a pointer type.  This file system never needs the priority.
 */
#undef PVM
#include <iprt/types.h>
#include <iprt/fs.h>
#include <VBox/shflsvc.h>
#include <VBox/VBoxGuestLibSharedFolders.h>

MALLOC_DECLARE(M_VBOXVFS);

#define	VBOXVFS_VFSNAME		"vboxvfs"

/* Defaults for mount options. */
#define	VBOXVFS_DEFAULT_TTL_MS	200
#define	VBOXVFS_DEFAULT_BIOSIZE	(64 * 1024)

/* Inode number of the root of a share; see vboxvfs_node_ino(). */
#define	VBOXVFS_ROOT_INO	2

/* Size of the buffer handed to SHFL_FN_LIST per request. */
#define	VBOXVFS_DIRBUF_SIZE	(16 * 1024)

/*
 * Debugging.  vfs.vboxvfs.debug selects the verbosity:
 *   0  nothing (default)
 *   1  errors that are not simply passed back to the caller
 *   2  one line per VOP
 *   3  host requests
 */
#define	VBOXVFS_DEBUG_ERR	1
#define	VBOXVFS_DEBUG_VOP	2
#define	VBOXVFS_DEBUG_HOST	3

extern int vboxvfs_debug;

#define	VBOXVFS_DPRINTF(level, ...) do {				\
	if (__predict_false(vboxvfs_debug >= (level)))			\
		printf("vboxvfs: " __VA_ARGS__);			\
} while (0)

SYSCTL_DECL(_vfs_vboxvfs);

/*
 * Per-mount state.
 */
struct vboxvfs_mount {
	struct mount		*vm_mp;
	SHFLROOT		 vm_root;	/* result of SHFL_FN_MAP_FOLDER */
	SHFLHANDLE		 vm_roothandle;	/* host handle of the root
						   directory, kept open for
						   statfs */
	struct vboxvfs_node	*vm_rootnode;	/* root vnode, referenced */
	uid_t			 vm_uid;	/* owner shown for everything */
	gid_t			 vm_gid;
	mode_t			 vm_fmode;	/* 0: use the host's mode */
	mode_t			 vm_dmode;
	mode_t			 vm_fmask;	/* bits cleared from host mode */
	mode_t			 vm_dmask;
	sbintime_t		 vm_ttl;	/* attribute cache lifetime */
	int			 vm_ttl_ms;
	u_int			 vm_biosize;	/* buffer cache block size */
	bool			 vm_hostro;	/* host says read-only share */
	uint32_t		 vm_namemax;	/* host's max name component */
	char			*vm_share;	/* share name */
};

/*
 * Per-file state, one per vnode.
 *
 * Locking: fields marked (v) are protected by the vnode lock, (m) by vn_mtx,
 * (c) are constant after creation.  vn_mtx is a leaf lock; it must not be
 * held across calls to the host.
 */
struct vboxvfs_node {
	struct vnode		*vn_vp;		/* (c) */
	struct vboxvfs_mount	*vn_vmp;	/* (c) */
	struct vboxvfs_node	*vn_parent;	/* (v) parent, holds a use
						   reference unless it is the
						   root; root points to self */
	char			*vn_name;	/* (v) last path component */
	u_int			 vn_namelen;	/* (v) */
	ino_t			 vn_ino;	/* (c) see vboxvfs_node_ino() */
	u_int			 vn_flags;	/* (m) VBOXVFS_NODE_* */

	/* Host handle. */
	SHFLHANDLE		 vn_handle;	/* (m) SHFL_HANDLE_NIL if none */
	uint32_t		 vn_haccess;	/* (m) SHFL_CF_ACCESS_* it was
						   opened with */
	int			 vn_opencnt;	/* (m) VOP_OPEN - VOP_CLOSE */

	/* Attribute cache. */
	SHFLFSOBJINFO		 vn_info;	/* (m) as reported by host */
	sbintime_t		 vn_info_time;	/* (m) when, 0 = invalid */
	uint64_t		 vn_size;	/* (m) size the VM knows about */
	int64_t			 vn_mtime_ns;	/* (m) mtime at last check */

	/* Directory listing cache, see vboxvfs_readdir(); (d) = vn_dirsx. */
	struct sx		 vn_dirsx;
	char			*vn_dirbuf;	/* (d) packed struct dirent */
	size_t			 vn_dirbuflen;	/* (d) bytes used */
	size_t			 vn_dirbufsize;	/* (d) bytes allocated */
	u_int			*vn_diroffs;	/* (d) offset of each entry */
	u_int			 vn_dircnt;	/* (d) entries, incl. . and .. */
	u_int			 vn_diroffcnt;	/* (d) slots in vn_diroffs */

	struct mtx		 vn_mtx;
};

/* vn_flags */
#define	VBOXVFS_NODE_ROOT	0x0001	/* the root of the share */
#define	VBOXVFS_NODE_REFPARENT	0x0002	/* holds a reference on vn_parent */
#define	VBOXVFS_NODE_STALE	0x0004	/* removed, or replaced by rename;
					   recycle when inactive */
#define	VBOXVFS_NODE_UNHASHED	0x0008	/* already taken out of vfs_hash */
#define	VBOXVFS_NODE_MODIFIED	0x0010	/* we wrote since the last revalidate;
					   do not drop the cache on the next
					   host mtime change */

#define	VFSTOVBOXVFS(mp)	((struct vboxvfs_mount *)((mp)->mnt_data))
#define	VTOVBOXVFS(vp)		((struct vboxvfs_node *)((vp)->v_data))
#define	VBOXVFSTOV(np)		((np)->vn_vp)

#define	VBOXVFS_NODE_LOCK(np)	mtx_lock(&(np)->vn_mtx)
#define	VBOXVFS_NODE_UNLOCK(np)	mtx_unlock(&(np)->vn_mtx)
#define	VBOXVFS_NODE_ASSERT(np)	mtx_assert(&(np)->vn_mtx, MA_OWNED)

/*
 * Flags for the SHFL_FN_CREATE wrapper, see vboxvfs_host_create().
 */
#define	VBOXVFS_CREATE_DIR_RO	(SHFL_CF_DIRECTORY |			\
	SHFL_CF_ACT_OPEN_IF_EXISTS | SHFL_CF_ACT_FAIL_IF_NEW |		\
	SHFL_CF_ACCESS_READ)

/*
 * vboxvfs_host.c -- requests to the host.  All functions return errno
 * values; the ones taking a path want it relative to the share root,
 * without a leading slash ("" is the root), not necessarily NUL terminated.
 */
int	vboxvfs_host_init(void);
void	vboxvfs_host_term(void);
int	vboxvfs_host_map(const char *share, SHFLROOT *rootp);
int	vboxvfs_host_unmap(SHFLROOT root);
int	vboxvfs_host_create(struct vboxvfs_mount *vmp, const char *path,
	    size_t pathlen, uint32_t cflags, mode_t mode,
	    SHFLCREATEPARMS *parms);
int	vboxvfs_host_close(struct vboxvfs_mount *vmp, SHFLHANDLE handle);
int	vboxvfs_host_lookup(struct vboxvfs_mount *vmp, const char *path,
	    size_t pathlen, SHFLFSOBJINFO *info);
int	vboxvfs_host_getinfo(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
	    SHFLFSOBJINFO *info);
int	vboxvfs_host_setinfo(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
	    SHFLFSOBJINFO *info);
int	vboxvfs_host_volinfo(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
	    SHFLVOLINFO *vi);
int	vboxvfs_host_list(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
	    uint32_t lflags, void *buf, uint32_t buflen, uint32_t *nentries,
	    bool *more);
void	*vboxvfs_host_alloc(size_t size);
void	vboxvfs_host_free(void *p);
int	vboxvfs_vrc2errno(int vrc);
int	vboxvfs_host_remove(struct vboxvfs_mount *vmp, const char *path,
	    size_t pathlen, uint32_t rflags);
int	vboxvfs_host_rename(struct vboxvfs_mount *vmp, const char *src,
	    size_t srclen, const char *dst, size_t dstlen, uint32_t rflags);
int	vboxvfs_host_readlink(struct vboxvfs_mount *vmp, const char *path,
	    size_t pathlen, char *buf, size_t bufsize, size_t *lenp);
int	vboxvfs_host_symlink(struct vboxvfs_mount *vmp, const char *path,
	    size_t pathlen, const char *target, size_t targetlen,
	    SHFLFSOBJINFO *info);
int	vboxvfs_host_setsize(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
	    uint64_t size);
int	vboxvfs_host_read(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
	    off_t off, struct buf *bp, u_int bufoff, size_t len, size_t *donep);
int	vboxvfs_host_write(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
	    off_t off, struct buf *bp, u_int bufoff, size_t len, size_t *donep);
int	vboxvfs_host_flush(struct vboxvfs_mount *vmp, SHFLHANDLE handle);
int	vboxvfs_host_lastfn(void);
uint32_t vboxvfs_host_features(void);

/*
 * vboxvfs_node.c -- node cache.
 */
int	vboxvfs_node_get(struct vboxvfs_mount *vmp, struct vnode *dvp,
	    const char *name, u_int namelen, const SHFLFSOBJINFO *info,
	    struct vnode **vpp);
void	vboxvfs_node_free(struct vboxvfs_node *np);
int	vboxvfs_node_path(struct vboxvfs_node *np, char **pathp,
	    size_t *lenp);
int	vboxvfs_node_childpath(struct vboxvfs_node *dnp, const char *name,
	    u_int namelen, char **pathp, size_t *lenp);
void	vboxvfs_node_freepath(char *path);
ino_t	vboxvfs_node_ino(const char *path, size_t len);
ino_t	vboxvfs_node_childino(const char *dirpath, size_t dirlen,
	    const char *name, u_int namelen);
void	vboxvfs_node_ref(struct vboxvfs_node *dnp, const char *name,
	    u_int namelen, struct vnode **vpp);
u_int	vboxvfs_node_hash(const struct vboxvfs_node *dnp, const char *name,
	    u_int namelen);
void	vboxvfs_node_setinfo(struct vboxvfs_node *np,
	    const SHFLFSOBJINFO *info);
int	vboxvfs_node_revalidate(struct vboxvfs_node *np, bool force);
void	vboxvfs_node_invalattr(struct vboxvfs_node *np);
void	vboxvfs_node_setsize(struct vnode *vp, uint64_t nsize);
int	vboxvfs_node_opendir(struct vboxvfs_node *np, SHFLHANDLE *handlep);
int	vboxvfs_node_handle(struct vboxvfs_node *np, uint32_t access,
	    SHFLHANDLE *handlep);
void	vboxvfs_node_remove(struct vboxvfs_node *np);
void	vboxvfs_node_rename(struct vboxvfs_node *np, struct vnode *tdvp,
	    const char *name, u_int namelen);
bool	vboxvfs_node_isunder(struct vboxvfs_node *np,
	    struct vboxvfs_node *ancestor);
void	vboxvfs_node_closehandle(struct vboxvfs_node *np);
void	vboxvfs_node_freedir(struct vboxvfs_node *np);
__enum_uint8(vtype) vboxvfs_mode2type(RTFMODE fmode);
mode_t	vboxvfs_mode(struct vboxvfs_mount *vmp, __enum_uint8(vtype) type,
	    RTFMODE fmode);

/*
 * vboxvfs_vnode.c
 */
extern struct vop_vector vboxvfs_vnodeops;

/*
 * vboxvfs_io.c -- data path through the buffer cache.
 */
vop_read_t	vboxvfs_read;
vop_write_t	vboxvfs_write;
vop_fsync_t	vboxvfs_fsync;
vop_getpages_t	vboxvfs_getpages;
int	vboxvfs_truncate(struct vnode *vp, uint64_t oldsize, uint64_t nsize);
vop_strategy_t	vboxvfs_strategy;

/*
 * Time conversion: the host uses nanoseconds since the Unix epoch.
 */
static __inline void
vboxvfs_ns2ts(int64_t ns, struct timespec *ts)
{

	ts->tv_sec = ns / 1000000000LL;
	ts->tv_nsec = ns % 1000000000LL;
	if (ts->tv_nsec < 0) {
		ts->tv_sec--;
		ts->tv_nsec += 1000000000L;
	}
}

static __inline int64_t
vboxvfs_ts2ns(const struct timespec *ts)
{

	return ((int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec);
}

#endif /* !_VBOXVFS_INTERNAL_H_ */
