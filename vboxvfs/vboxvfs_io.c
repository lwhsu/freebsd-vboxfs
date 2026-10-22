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

/*
 * Data written behind (partial blocks that are not yet complete) is flushed
 * to the host by the syncer this many seconds after the write rather than
 * after its usual thirty, so that the host sees a growing file soon; 0
 * leaves it to the syncer.  Complete blocks go out at once either way.
 */
static int vboxvfs_writedelay = 1;
SYSCTL_INT(_vfs_vboxvfs, OID_AUTO, writedelay, CTLFLAG_RWTUN,
    &vboxvfs_writedelay, 0,
    "Seconds within which data written behind reaches the host");

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
 * Get a host handle for the buffer I/O below.  The buffer is locked and its
 * pages are busy, so the attributes that come with a freshly opened handle
 * must not drop the cache or resize the VM object (vboxvfs_node_update()
 * would wait for this very buffer): vn_inio makes it treat them as it does
 * an answer that predates our own writes.
 */
static int
vboxvfs_doio_handle(struct vboxvfs_node *np, uint32_t access,
    SHFLHANDLE *handlep)
{
	int error;

	VBOXVFS_NODE_LOCK(np);
	np->vn_inio++;
	VBOXVFS_NODE_UNLOCK(np);
	error = vboxvfs_node_handle(np, access, handlep, NULL);
	VBOXVFS_NODE_LOCK(np);
	np->vn_inio--;
	VBOXVFS_NODE_UNLOCK(np);
	return (error);
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
		error = vboxvfs_doio_handle(np, SHFL_CF_ACCESS_READ, &handle);
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
		/*
		 * Only the dirty range is written, clipped to the file size.
		 * The write is in flight from here on (vboxvfs_node_update()
		 * must not trust host answers that may predate it); the caller
		 * may be the syncer or the buffer daemon, not VOP_WRITE.
		 */
		vboxvfs_node_modstart(np);
		VBOXVFS_NODE_LOCK(np);
		size = np->vn_size;
		VBOXVFS_NODE_UNLOCK(np);
		if ((off_t)bp->b_blkno * DEV_BSIZE + bp->b_dirtyend > (off_t)size)
			bp->b_dirtyend = size - (off_t)bp->b_blkno * DEV_BSIZE;
		if (bp->b_dirtyend > bp->b_dirtyoff) {
			off = (off_t)bp->b_blkno * DEV_BSIZE + bp->b_dirtyoff;
			len = bp->b_dirtyend - bp->b_dirtyoff;
			error = vboxvfs_doio_handle(np, SHFL_CF_ACCESS_WRITE,
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
		} else {
			/*
			 * A failed write that nobody waits for (delayed, async)
			 * would be retried forever by brelse(); drop the data
			 * and report the error at the next close or fsync.
			 */
			bp->b_flags |= B_INVAL | B_NOCACHE;
			VBOXVFS_NODE_LOCK(np);
			if (np->vn_werror == 0)
				np->vn_werror = error;
			/* The size is the host's again; ask. */
			np->vn_info_time = 0;
			VBOXVFS_NODE_UNLOCK(np);
		}
		vboxvfs_node_modend(np);
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
	sbintime_t now;
	int bcount, biosize, delay, error, error1, ioflag, n, obcount, on;
	uint32_t save;
	bool bp_cached, delayed, extended, sched;

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
	delayed = false;
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
		/*
		 * Through to the host right away when asked (O_SYNC, fsync-like
		 * IO_SYNC, the "sync" mount option); otherwise behind, the way
		 * the NFS client does it: a block that is complete goes out now,
		 * a partial one stays dirty until a later write completes it,
		 * close(2), fsync(2), msync(2), the syncer or memory pressure.
		 * Sixteen 4 KiB writes then cost one host request, not sixteen.
		 */
		if ((ioflag & IO_SYNC) != 0 ||
		    (vp->v_mount->mnt_flag & MNT_SYNCHRONOUS) != 0) {
			error1 = bwrite(bp);
			if (error1 != 0 && error == 0)
				error = error1;
		} else if (on + n == biosize || (ioflag & IO_ASYNC) != 0)
			bawrite(bp);
		else {
			bdwrite(bp);
			delayed = true;
		}
	} while (error == 0 && uio->uio_resid > 0);

	vboxvfs_node_modend(np);
	vboxvfs_node_changed(np);
	/*
	 * Ask the syncer to come by early for what stays dirty.  A slot of
	 * delay - 1 is visited within delay seconds; asking again moves the
	 * node to a new slot, so a steady writer asks at most once per delay
	 * (the syncer puts a node that is still dirty after its visit thirty
	 * seconds out again), and not at all while an earlier request is
	 * pending.
	 */
	delay = vboxvfs_writedelay;
	if (delayed && delay > 0) {
		now = getsbinuptime();
		VBOXVFS_NODE_LOCK(np);
		sched = now - np->vn_flushreq >= delay * SBT_1S;
		if (sched)
			np->vn_flushreq = now;
		VBOXVFS_NODE_UNLOCK(np);
		if (sched) {
			BO_LOCK(&vp->v_bufobj);
			vn_syncer_add_to_worklist(&vp->v_bufobj, delay - 1);
			BO_UNLOCK(&vp->v_bufobj);
		}
	}
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
	error = vboxvfs_flushbufs(vp, ap->a_waitfor, true);
	/* The syncer's periodic visit does not warrant an fsync on the host. */
	if (error != 0 || vp->v_type != VREG || ap->a_waitfor == MNT_LAZY)
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

/*
 * Write the dirty buffers of a regular file to the host (close-to-open:
 * called at close(2), before the host handle is given back, and by fsync),
 * and, if asked, report an error a delayed write ran into meanwhile.  The
 * vnode is locked.
 */
int
vboxvfs_flushbufs(struct vnode *vp, int waitfor, bool report)
{
	struct vop_fsync_args a;
	struct vboxvfs_node *np;
	int error, werror;
	bool clean;

	np = VTOVBOXVFS(vp);
	error = 0;
	if (vp->v_bufobj.bo_dirty.bv_cnt > 0 || vp->v_bufobj.bo_numoutput > 0) {
		a.a_gen.a_desc = &vop_fsync_desc;
		a.a_vp = vp;
		a.a_waitfor = waitfor;
		a.a_td = curthread;
		error = vop_stdfsync(&a);
	}
	/* Clean again: the next write behind asks the syncer afresh. */
	BO_LOCK(&vp->v_bufobj);
	clean = vp->v_bufobj.bo_dirty.bv_cnt == 0;
	BO_UNLOCK(&vp->v_bufobj);
	if (clean) {
		VBOXVFS_NODE_LOCK(np);
		np->vn_flushreq = 0;
		VBOXVFS_NODE_UNLOCK(np);
	}
	werror = 0;
	if (report) {
		VBOXVFS_NODE_LOCK(np);
		werror = np->vn_werror;
		np->vn_werror = 0;
		VBOXVFS_NODE_UNLOCK(np);
	}
	return (error != 0 ? error : werror);
}

/*
 * The file is about to be removed or replaced by a rename: data written
 * behind is dropped when nothing but the caller refers to the file (nobody
 * will see it by name), and goes to the host first when the file is still
 * open or mapped, since reads through those may need it later.  The vnode
 * is locked.
 */
void
vboxvfs_dropbufs(struct vnode *vp)
{

	if (vp->v_type != VREG)
		return;
	if (vrefcnt(vp) > 1)
		(void)vboxvfs_flushbufs(vp, MNT_WAIT, false);
	else
		(void)vinvalbuf(vp, 0, 0, 0);
}
