/*-
 * SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
 *
 * Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
 */

/*
 * vboxvfs -- the data path: read, write, fsync and strategy through the
 * buffer cache, the way nfsclient and smbfs do it.  Reads are cached and
 * revalidated against the host; writes go to the host immediately
 * (write-through) but stay in the cache, so read(2), write(2) and mmap(2)
 * all see the same pages.  The default VOP_GETPAGES/VOP_PUTPAGES
 * implementations end up here through VOP_BMAP (the default identity
 * mapping) and VOP_STRATEGY / VOP_WRITE.
 */

#include "vboxvfs_internal.h"

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/fcntl.h>
#include <sys/limits.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/uio.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_extern.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

/*
 * getblk() wrapper: block numbers are file offsets in DEV_BSIZE units,
 * matching what vop_stdbmap() hands to the vnode pager.
 */
static struct buf *
vboxvfs_getcacheblk(struct vnode *vp, daddr_t lbn, int size)
{
	struct buf *bp;

	bp = getblk(vp, lbn, size, 0, 0, 0);
	if (bp != NULL && vp->v_type == VREG)
		bp->b_blkno = lbn * (vp->v_bufobj.bo_bsize / DEV_BSIZE);
	return (bp);
}

/*
 * Transfer one buffer to or from the host and complete it.
 */
static int
vboxvfs_doio(struct vnode *vp, struct buf *bp)
{
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *np;
	SHFLHANDLE handle;
	off_t off;
	size_t done, len;
	uint64_t size;
	int error;

	np = VTOVBOXVFS(vp);
	vmp = np->vn_vmp;
	bp->b_flags &= ~B_INVAL;
	bp->b_ioflags &= ~BIO_ERROR;
	done = 0;

	if (bp->b_iocmd == BIO_READ) {
		off = (off_t)bp->b_blkno * DEV_BSIZE;
		len = bp->b_bcount;
		error = vboxvfs_node_handle(np, SHFL_CF_ACCESS_READ, &handle);
		if (error == 0)
			error = vboxvfs_host_read(vmp, handle, off, bp, 0, len,
			    &done);
		if (error == 0) {
			/* Short read: end of file or a hole, zero the rest. */
			if (done < len)
				vfs_bio_bzero_buf(bp, done, len - done);
			bp->b_resid = 0;
		}
	} else {
		/* Only the dirty range is written, clipped to the file size. */
		VBOXVFS_NODE_LOCK(np);
		size = np->vn_size;
		VBOXVFS_NODE_UNLOCK(np);
		if ((off_t)bp->b_blkno * DEV_BSIZE + bp->b_dirtyend > (off_t)size)
			bp->b_dirtyend = size - (off_t)bp->b_blkno * DEV_BSIZE;
		if (bp->b_dirtyend > bp->b_dirtyoff) {
			off = (off_t)bp->b_blkno * DEV_BSIZE + bp->b_dirtyoff;
			len = bp->b_dirtyend - bp->b_dirtyoff;
			error = vboxvfs_node_handle(np, SHFL_CF_ACCESS_WRITE,
			    &handle);
			if (error == 0)
				error = vboxvfs_host_write(vmp, handle, off, bp,
				    bp->b_dirtyoff, len, &done);
			if (error == 0 && done != len) {
				VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR,
				    "short write at %jd: %zu of %zu\n",
				    (intmax_t)off, done, len);
				error = EIO;
			}
		} else
			error = 0;
		if (error == 0) {
			bp->b_dirtyoff = bp->b_dirtyend = 0;
			bp->b_resid = 0;
		}
	}
	if (error != 0) {
		bp->b_error = error;
		bp->b_ioflags |= BIO_ERROR;
		bp->b_resid = bp->b_bcount;
	}
	bufdone(bp);
	return (error);
}

int
vboxvfs_strategy(struct vop_strategy_args *ap)
{
	struct vnode *vp;
	struct buf *bp;

	vp = ap->a_vp;
	bp = ap->a_bp;
	/*
	 * Buffers from bread() (vfs_bio_getpages) still carry the logical
	 * block number; translate it the way vop_stdbmap() does.
	 */
	if (bp->b_blkno == bp->b_lblkno)
		bp->b_blkno = bp->b_lblkno *
		    (vp->v_mount->mnt_stat.f_iosize / DEV_BSIZE);
	if (vp->v_type != VREG) {
		bp->b_ioflags |= BIO_ERROR;
		bp->b_error = EOPNOTSUPP;
		bufdone(bp);
		return (0);
	}
	(void)vboxvfs_doio(ap->a_vp, bp);
	return (0);
}

int
vboxvfs_read(struct vop_read_args *ap)
{
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *np;
	struct vnode *vp;
	struct uio *uio;
	struct buf *bp;
	daddr_t lbn;
	off_t nsize;
	int bcount, biosize, error, n, on;

	vp = ap->a_vp;
	uio = ap->a_uio;
	np = VTOVBOXVFS(vp);
	vmp = np->vn_vmp;

	switch (vp->v_type) {
	case VREG:
		break;
	case VDIR:
		return (EISDIR);
	default:
		return (EINVAL);
	}
	if (uio->uio_resid == 0)
		return (0);
	if (uio->uio_offset < 0)
		return (EINVAL);

	/* Revalidate (ttl); a host-side change drops the cached pages. */
	error = vboxvfs_node_revalidate(np, false);
	if (error != 0)
		return (error);
	biosize = vmp->vm_biosize;
	n = 0;
	do {
		VBOXVFS_NODE_LOCK(np);
		nsize = np->vn_size;
		VBOXVFS_NODE_UNLOCK(np);
		if (uio->uio_offset >= nsize)
			break;
		lbn = uio->uio_offset / biosize;
		on = uio->uio_offset - (off_t)lbn * biosize;
		bcount = biosize;
		if ((off_t)(lbn + 1) * biosize > nsize)
			bcount = nsize - (off_t)lbn * biosize;

		bp = vboxvfs_getcacheblk(vp, lbn, bcount);
		if (bp == NULL)
			return (EINTR);
		if ((bp->b_flags & B_CACHE) == 0) {
			bp->b_iocmd = BIO_READ;
			vfs_busy_pages(bp, 0);
			error = vboxvfs_doio(vp, bp);
			if (error != 0) {
				brelse(bp);
				return (error);
			}
		}
		n = MIN(uio->uio_resid, bcount - on);
		if (n > 0)
			error = vn_io_fault_uiomove(bp->b_data + on, n, uio);
		brelse(bp);
	} while (error == 0 && uio->uio_resid > 0 && n > 0);
	return (error);
}

int
vboxvfs_write(struct vop_write_args *ap)
{
	struct vboxvfs_mount *vmp;
	struct vboxvfs_node *np;
	struct vnode *vp;
	struct uio *uio;
	struct buf *bp;
	daddr_t lbn;
	ssize_t local_resid;
	off_t nsize;
	int bcount, biosize, error, error1, ioflag, n, obcount, on;
	uint32_t save;
	bool bp_cached, extended;

	vp = ap->a_vp;
	uio = ap->a_uio;
	ioflag = ap->a_ioflag;
	np = VTOVBOXVFS(vp);
	vmp = np->vn_vmp;

	if (vp->v_type != VREG)
		return (vp->v_type == VDIR ? EISDIR : EINVAL);
	if (uio->uio_offset < 0)
		return (EINVAL);
	if (uio->uio_resid == 0)
		return (0);
	if (ioflag & IO_APPEND) {
		/* The size may have changed on the host. */
		error = vboxvfs_node_revalidate(np, true);
		if (error != 0)
			return (error);
		VBOXVFS_NODE_LOCK(np);
		uio->uio_offset = np->vn_size;
		VBOXVFS_NODE_UNLOCK(np);
	}
	if (uio->uio_segflg != UIO_NOCOPY) {
		error = vn_rlimit_fsize(vp, uio, uio->uio_td);
		if (error != 0)
			return (error);
	}

	/*
	 * Attributes the host reports while this write is in flight may predate
	 * it; vboxvfs_node_update() must not shrink the file or drop pages on
	 * their account (a page in a shared mapping would fault with SIGBUS).
	 */
	vboxvfs_node_modstart(np);
	biosize = vmp->vm_biosize;
	error = 0;
	do {
		lbn = uio->uio_offset / biosize;
		on = uio->uio_offset - (off_t)lbn * biosize;
		n = MIN((unsigned)(biosize - on), uio->uio_resid);

		VBOXVFS_NODE_LOCK(np);
		nsize = np->vn_size;
		VBOXVFS_NODE_UNLOCK(np);
		if (uio->uio_offset == nsize && n > 0 &&
		    (obcount = nsize - (off_t)lbn * biosize) > 0) {
			/*
			 * Appending to a block that already has data: get the
			 * buffer at its old size so that B_CACHE survives, then
			 * grow the file and the buffer.  Without this every
			 * append would read the block back from the host first.
			 */
			bp = vboxvfs_getcacheblk(vp, lbn, obcount);
			if (bp == NULL) {
				error = EINTR;
				break;
			}
			VBOXVFS_NODE_LOCK(np);
			np->vn_size = nsize + n;
			np->vn_flags |= VBOXVFS_NODE_MODIFIED;
			nsize = np->vn_size;
			VBOXVFS_NODE_UNLOCK(np);
			vnode_pager_setsize(vp, nsize);
			save = bp->b_flags & B_CACHE;
			bcount = obcount + n;
			allocbuf(bp, bcount);
			bp->b_flags |= save;
		} else {
			/* Extend the file first so that getblk() sees the size. */
			extended = false;
			VBOXVFS_NODE_LOCK(np);
			if (uio->uio_offset + n > (off_t)np->vn_size) {
				np->vn_size = uio->uio_offset + n;
				extended = true;
			}
			nsize = np->vn_size;
			np->vn_flags |= VBOXVFS_NODE_MODIFIED;
			VBOXVFS_NODE_UNLOCK(np);
			if (extended)
				vnode_pager_setsize(vp, nsize);
			bcount = biosize;
			if ((off_t)(lbn + 1) * biosize > nsize)
				bcount = nsize - (off_t)lbn * biosize;
			bp = vboxvfs_getcacheblk(vp, lbn, bcount);
			if (bp == NULL) {
				error = EINTR;
				break;
			}
		}
		/*
		 * A write covering the whole buffer needs no read; otherwise
		 * fetch the block first unless it is cached.
		 */
		bp_cached = true;
		if (on == 0 && n == bcount) {
			if ((bp->b_flags & B_CACHE) == 0)
				bp_cached = false;
			bp->b_flags |= B_CACHE;
			bp->b_flags &= ~B_INVAL;
			bp->b_ioflags &= ~BIO_ERROR;
		}
		if ((bp->b_flags & B_CACHE) == 0) {
			bp->b_iocmd = BIO_READ;
			vfs_busy_pages(bp, 0);
			error = vboxvfs_doio(vp, bp);
			if (error != 0) {
				brelse(bp);
				break;
			}
		}
		if (bp->b_dirtyend > bcount)
			bp->b_dirtyend = bcount;
		if (bp->b_dirtyoff >= bp->b_dirtyend)
			bp->b_dirtyoff = bp->b_dirtyend = 0;

		local_resid = uio->uio_resid;
		error = vn_io_fault_uiomove((char *)bp->b_data + on, n, uio);
		if (error != 0 && !bp_cached) {
			/* Nothing valid in this buffer; drop it. */
			bp->b_ioflags |= BIO_ERROR;
			brelse(bp);
			uio->uio_offset -= local_resid - uio->uio_resid;
			uio->uio_resid = local_resid;
			break;
		}
		if (error != 0)
			n = local_resid - uio->uio_resid;
		if (n > 0) {
			if (bp->b_dirtyend > 0) {
				bp->b_dirtyoff = MIN(on, bp->b_dirtyoff);
				bp->b_dirtyend = MAX(on + n, bp->b_dirtyend);
			} else {
				bp->b_dirtyoff = on;
				bp->b_dirtyend = on + n;
			}
			vfs_bio_set_valid(bp, on, n);
		}
		/* Write-through: the host sees every write(2) right away. */
		error1 = bwrite(bp);
		if (error1 != 0 && error == 0)
			error = error1;
	} while (error == 0 && uio->uio_resid > 0);

	vboxvfs_node_modend(np);
	/* The host's mtime has changed; the next getattr fetches it. */
	vboxvfs_node_invalattr(np);
	return (error);
}

int
vboxvfs_fsync(struct vop_fsync_args *ap)
{
	struct vboxvfs_node *np;
	struct vnode *vp;
	SHFLHANDLE handle;
	int error;

	vp = ap->a_vp;
	np = VTOVBOXVFS(vp);
	error = vop_stdfsync(ap);
	if (error != 0 || vp->v_type != VREG)
		return (error);
	VBOXVFS_NODE_LOCK(np);
	handle = np->vn_handle;
	VBOXVFS_NODE_UNLOCK(np);
	if (handle != SHFL_HANDLE_NIL)
		error = vboxvfs_host_flush(np->vn_vmp, handle);
	return (error);
}

/*
 * Page-in through the buffer cache (vfs_bio_getpages(), as ufs, ext2fs
 * and msdosfs do).  vnode_pager_generic_getpages() cannot be used with a
 * 64 KiB block size, and going through the buffers keeps mmap and
 * read(2)/write(2) on the very same pages.
 */
static daddr_t
vboxvfs_gbp_getblkno(struct vnode *vp, vm_ooffset_t off)
{

	return (off / VTOVBOXVFS(vp)->vn_vmp->vm_biosize);
}

static int
vboxvfs_gbp_getblksz(struct vnode *vp, daddr_t lbn, long *sz)
{
	struct vboxvfs_node *np;
	off_t nsize;
	long biosize;

	np = VTOVBOXVFS(vp);
	biosize = np->vn_vmp->vm_biosize;
	VBOXVFS_NODE_LOCK(np);
	nsize = np->vn_size;
	VBOXVFS_NODE_UNLOCK(np);
	/* The last block of the file is as long as the data in it. */
	if ((off_t)(lbn + 1) * biosize > nsize && nsize > (off_t)lbn * biosize)
		*sz = nsize - (off_t)lbn * biosize;
	else
		*sz = biosize;
	return (0);
}

int
vboxvfs_getpages(struct vop_getpages_args *ap)
{

	return (vfs_bio_getpages(ap->a_vp, ap->a_m, ap->a_count, ap->a_rbehind,
	    ap->a_rahead, vboxvfs_gbp_getblkno, vboxvfs_gbp_getblksz));
}

/*
 * Size change bookkeeping for the cache.  Shrinking: vtruncbuf() drops the
 * buffers beyond the new end but not the one overlapping it, which may still
 * carry cached (now stale) data past the new size with B_CACHE set, so
 * fetch it at its new, shorter size and release it (nfsclient's
 * ncl_meta_setsize()).  Growing only needs the pager told.  Called with the
 * vnode locked exclusively, after the host accepted the new size.
 */
int
vboxvfs_truncate(struct vnode *vp, uint64_t oldsize, uint64_t nsize)
{
	struct buf *bp;
	daddr_t lbn;
	int biosize, bufsize, error;

	biosize = VTOVBOXVFS(vp)->vn_vmp->vm_biosize;
	if (nsize < oldsize) {
		error = vtruncbuf(vp, nsize, biosize);
		lbn = nsize / biosize;
		bufsize = nsize - (off_t)lbn * biosize;
		bp = vboxvfs_getcacheblk(vp, lbn, bufsize);
		if (bp == NULL)
			return (EINTR);
		if (bp->b_dirtyoff > bp->b_bcount)
			bp->b_dirtyoff = bp->b_bcount;
		if (bp->b_dirtyend > bp->b_bcount)
			bp->b_dirtyend = bp->b_bcount;
		bp->b_flags |= B_RELBUF;
		brelse(bp);
		return (error);
	}
	vnode_pager_setsize(vp, nsize);
	return (0);
}
