/*-
 * SPDX-License-Identifier: GPL-3.0-only OR BSD-2-Clause
 *
 * Copyright (c) 2026 Li-Wen Hsu <lwhsu@FreeBSD.org>
 */

/*
 * vboxvfs -- thin wrappers around the VirtualBox guest library's shared
 * folder requests (VBoxGuestLibSharedFoldersInline.h): request allocation
 * on the physical heap, path strings, error conversion and host feature
 * detection.  This is the only file that includes the inline request
 * header.
 */

#include "vboxvfs_internal.h"

#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/stat.h>

#include <vm/vm.h>
#include <vm/pmap.h>
#include <vm/vm_page.h>

#include <iprt/err.h>
#include <VBox/err.h>
#include <VBox/VBoxGuestLib.h>
#include <VBox/VBoxGuestLibSharedFoldersInline.h>
#include <iprt/log.h>

/*
 * Required by VBoxGuestLibSharedFoldersInline.h: the HGCM client and the
 * VMMDev feature flags, shared by all mounts.
 */
VBGLSFCLIENT g_SfClient;
uint32_t g_fHostFeatures;

static uint64_t vboxvfs_sffeatures;
static int vboxvfs_nobounce = 1;
SYSCTL_INT(_vfs_vboxvfs, OID_AUTO, nobounce, CTLFLAG_RDTUN, &vboxvfs_nobounce, 0,
    "Use no-bounce HGCM page lists when the host offers them (tunable)");
static uint32_t vboxvfs_sflastfn;
static bool vboxvfs_connected;

/*
 * Convert an IPRT status code to an errno value.
 */
int
vboxvfs_vrc2errno(int vrc)
{

	if (RT_SUCCESS(vrc))
		return (0);
	switch (vrc) {
	case VERR_FILE_NOT_FOUND:
	case VERR_PATH_NOT_FOUND:
	case VERR_NOT_FOUND:
	case VERR_INVALID_NAME:		/* bad name for the host file system */
		return (ENOENT);
	case VERR_ACCESS_DENIED:
	case VERR_PERMISSION_DENIED:
		return (EACCES);
	case VERR_ALREADY_EXISTS:
		return (EEXIST);
	case VERR_DIR_NOT_EMPTY:
		return (ENOTEMPTY);
	case VERR_NOT_A_DIRECTORY:
		return (ENOTDIR);
	case VERR_IS_A_DIRECTORY:
		return (EISDIR);
	case VERR_WRITE_PROTECT:
		return (EROFS);
	case VERR_DISK_FULL:
		return (ENOSPC);
	case VERR_FILENAME_TOO_LONG:
		return (ENAMETOOLONG);
	case VERR_SHARING_VIOLATION:
	case VERR_RESOURCE_BUSY:
	case VERR_FILE_LOCK_VIOLATION:
	case VERR_FILE_LOCK_FAILED:
		return (EBUSY);
	case VERR_TOO_MANY_OPEN_FILES:
		return (ENFILE);
	case VERR_INVALID_HANDLE:
		return (EBADF);
	case VERR_INVALID_PARAMETER:
	case VERR_INVALID_POINTER:
	case VERR_INVALID_FUNCTION:
	case VERR_OUT_OF_RANGE:
	case VERR_INVALID_UTF8_ENCODING:
		return (EINVAL);
	case VERR_NO_MEMORY:
		return (ENOMEM);
	case VERR_NOT_SUPPORTED:
	case VERR_NOT_IMPLEMENTED:
		return (EOPNOTSUPP);
	case VERR_INTERRUPTED:
		return (EINTR);
	case VERR_TIMEOUT:
		return (ETIMEDOUT);
	case VERR_FILE_TOO_BIG:
		return (EFBIG);
	case VERR_NOT_SAME_DEVICE:
		return (EXDEV);
	case VERR_TOO_MANY_SYMLINKS:
		return (ELOOP);
	case VERR_BUFFER_OVERFLOW:
		return (EOVERFLOW);
	case VERR_HGCM_SERVICE_NOT_FOUND:
		return (ENXIO);
	case VERR_SEEK:
	case VERR_EOF:
	case VERR_NO_MORE_FILES:
	case VERR_DEV_IO_ERROR:
	case VERR_READ_ERROR:
	case VERR_WRITE_ERROR:
	case VERR_FILE_IO_ERROR:
	default:
		VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR,
		    "unmapped host status %d, returning EIO\n", vrc);
		return (EIO);
	}
}

void *
vboxvfs_host_alloc(size_t size)
{

	return (VbglR0PhysHeapAlloc(size));
}

void
vboxvfs_host_free(void *p)
{

	if (p != NULL)
		VbglR0PhysHeapFree(p);
}

int
vboxvfs_host_lastfn(void)
{

	return (vboxvfs_sflastfn);
}

uint32_t
vboxvfs_host_features(void)
{

	return (g_fHostFeatures);
}

/*
 * Connect to the shared folder service.  Called once at module load.
 */
int
vboxvfs_host_init(void)
{
	int vrc;

	vrc = VbglR0SfInit();
	if (RT_FAILURE(vrc)) {
		VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR, "VbglR0SfInit: %d\n", vrc);
		return (vboxvfs_vrc2errno(vrc));
	}
	vrc = VbglR0SfConnect(&g_SfClient);
	if (RT_FAILURE(vrc)) {
		VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR, "VbglR0SfConnect: %d\n",
		    vrc);
		VbglR0SfTerm();
		return (vboxvfs_vrc2errno(vrc));
	}
	vboxvfs_connected = true;

	vrc = VbglR0QueryHostFeatures(&g_fHostFeatures);
	if (RT_FAILURE(vrc)) {
		VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR,
		    "VbglR0QueryHostFeatures: %d (ignored)\n", vrc);
		g_fHostFeatures = 0;
	}
	/*
	 * With VMMDEV_HVF_HGCM_NO_BOUNCE_PAGE_LIST the inline request builders
	 * hand the host our pages for direct access instead of having it
	 * copy; the vfs.vboxvfs.nobounce tunable can turn that off for
	 * debugging (a bogus page address shows up as VERR_INTERNAL_ERROR
	 * with no-bounce lists and as silent garbage with plain ones).
	 */
	if (vboxvfs_nobounce == 0)
		g_fHostFeatures &= ~VMMDEV_HVF_HGCM_NO_BOUNCE_PAGE_LIST;
	/* Must come after the VMMDev query, it falls back on it. */
	(void)VbglR0SfHostReqQueryFeaturesSimple(&vboxvfs_sffeatures,
	    &vboxvfs_sflastfn);

	vrc = VbglR0SfHostReqSetUtf8Simple();
	if (RT_FAILURE(vrc)) {
		VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR, "SET_UTF8: %d\n", vrc);
		vboxvfs_host_term();
		return (vboxvfs_vrc2errno(vrc));
	}
	/* Report symbolic links instead of following them on the host. */
	vrc = VbglR0SfHostReqSetSymlinksSimple();
	if (RT_FAILURE(vrc))
		printf("vboxvfs: host does not support showing symbolic "
		    "links (%d)\n", vrc);
	/* POSIX style error codes (ENOTDIR vs. ENOENT) from Windows hosts. */
	if (vboxvfs_sflastfn >= SHFL_FN_SET_ERROR_STYLE)
		(void)VbglR0SfHostReqSetErrorStyleSimple(kShflErrorStyle_Linux);

	printf("vboxvfs: host features %#x, shared folder features %#jx, "
	    "last function %u\n", g_fHostFeatures,
	    (uintmax_t)vboxvfs_sffeatures, vboxvfs_sflastfn);
	return (0);
}

void
vboxvfs_host_term(void)
{

	if (vboxvfs_connected) {
		VbglR0SfDisconnect(&g_SfClient);
		vboxvfs_connected = false;
	}
	VbglR0SfTerm();
}

/*
 * The debug builds of the guest libraries create IPRT's default logger the
 * first time one of their Log() statements runs (inside VbglR0SfInit()),
 * and nothing in VbglR0SfTerm() destroys it, which shows up as "memory type
 * iprtheap leaked memory" at kldunload.  Release builds never create one
 * and both calls are no-ops then.  Called last, after the libraries are
 * done logging.
 */
void
vboxvfs_host_destroylog(void)
{

	RTLogDestroy(RTLogSetDefaultInstance(NULL));
	RTLogDestroy(RTLogRelSetDefaultInstance(NULL));
}

/*
 * Fill in a SHFLSTRING from a path that is not necessarily NUL terminated.
 */
static void
vboxvfs_host_setstring(SHFLSTRING *str, const char *path, size_t pathlen)
{

	str->u16Length = pathlen;
	str->u16Size = pathlen + 1;
	bcopy(path, str->String.ach, pathlen);
	str->String.ach[pathlen] = '\0';
}

int
vboxvfs_host_map(const char *share, SHFLROOT *rootp)
{
	SHFLSTRING *str;
	size_t len;
	int vrc;

	len = strlen(share);
	str = VbglR0PhysHeapAlloc(SHFLSTRING_HEADER_SIZE + len + 1);
	if (str == NULL)
		return (ENOMEM);
	vboxvfs_host_setstring(str, share, len);
	vrc = VbglR0SfHostReqMapFolderWithContigSimple(str,
	    VbglR0PhysHeapGetPhysAddr(str), '/', true /* case sensitive */,
	    rootp);
	VbglR0PhysHeapFree(str);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "MAP_FOLDER \"%s\": %d root %u\n",
	    share, vrc, *rootp);
	if (vrc == VERR_FILE_NOT_FOUND || vrc == VERR_PATH_NOT_FOUND)
		return (ENXIO);
	return (vboxvfs_vrc2errno(vrc));
}

int
vboxvfs_host_unmap(SHFLROOT root)
{
	int vrc;

	vrc = VbglR0SfHostReqUnmapFolderSimple(root);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "UNMAP_FOLDER %u: %d\n", root, vrc);
	return (vboxvfs_vrc2errno(vrc));
}

/*
 * SHFL_FN_CREATE: open, create, mkdir or (with SHFL_CF_LOOKUP) just query
 * an object.  On success *parms tells what happened; parms->Handle is
 * SHFL_HANDLE_NIL when nothing was opened.
 */
int
vboxvfs_host_create(struct vboxvfs_mount *vmp, const char *path,
    size_t pathlen, uint32_t cflags, mode_t mode, SHFLCREATEPARMS *parms)
{
	VBOXSFCREATEREQ *req;
	int vrc;

	if (pathlen >= UINT16_MAX)
		return (ENAMETOOLONG);
	req = VbglR0PhysHeapAlloc(RT_UOFFSETOF(VBOXSFCREATEREQ,
	    StrPath.String) + pathlen + 1);
	if (req == NULL)
		return (ENOMEM);
	vboxvfs_host_setstring(&req->StrPath, path, pathlen);
	RT_ZERO(req->CreateParms);
	req->CreateParms.Handle = SHFL_HANDLE_NIL;
	req->CreateParms.CreateFlags = cflags;
	if ((cflags & SHFL_CF_LOOKUP) == 0) {
		req->CreateParms.Info.Attr.fMode =
		    ((cflags & SHFL_CF_DIRECTORY) ? RTFS_TYPE_DIRECTORY :
		    RTFS_TYPE_FILE) | (mode & ALLPERMS);
		req->CreateParms.Info.Attr.enmAdditional =
		    SHFLFSOBJATTRADD_NOTHING;
	}
	vrc = VbglR0SfHostReqCreate(vmp->vm_root, req);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST,
	    "CREATE \"%.*s\" flags %#x: %d result %d handle %#jx\n",
	    (int)pathlen, path, cflags, vrc, (int)req->CreateParms.Result,
	    (uintmax_t)req->CreateParms.Handle);
	if (RT_SUCCESS(vrc))
		*parms = req->CreateParms;
	VbglR0PhysHeapFree(req);
	return (vboxvfs_vrc2errno(vrc));
}

int
vboxvfs_host_close(struct vboxvfs_mount *vmp, SHFLHANDLE handle)
{
	int vrc;

	vrc = VbglR0SfHostReqCloseSimple(vmp->vm_root, handle);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "CLOSE %#jx: %d\n",
	    (uintmax_t)handle, vrc);
	return (vboxvfs_vrc2errno(vrc));
}

/*
 * Query the attributes of a path without opening it.  ENOENT if it does not
 * exist.
 */
int
vboxvfs_host_lookup(struct vboxvfs_mount *vmp, const char *path,
    size_t pathlen, SHFLFSOBJINFO *info)
{
	SHFLCREATEPARMS parms;
	int error;

	error = vboxvfs_host_create(vmp, path, pathlen, SHFL_CF_LOOKUP, 0,
	    &parms);
	if (error != 0)
		return (error);
	switch (parms.Result) {
	case SHFL_FILE_EXISTS:
		*info = parms.Info;
		return (0);
	case SHFL_FILE_NOT_FOUND:
	case SHFL_PATH_NOT_FOUND:
		return (ENOENT);
	default:
		VBOXVFS_DPRINTF(VBOXVFS_DEBUG_ERR,
		    "lookup of \"%.*s\": unexpected result %d\n", (int)pathlen,
		    path, (int)parms.Result);
		return (EIO);
	}
}

int
vboxvfs_host_getinfo(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
    SHFLFSOBJINFO *info)
{
	VBOXSFOBJINFOREQ *req;
	int vrc;

	req = VbglR0PhysHeapAlloc(sizeof(*req));
	if (req == NULL)
		return (ENOMEM);
	vrc = VbglR0SfHostReqQueryObjInfo(vmp->vm_root, req, handle);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "QUERY_OBJ_INFO %#jx: %d\n",
	    (uintmax_t)handle, vrc);
	if (RT_SUCCESS(vrc))
		*info = req->ObjInfo;
	VbglR0PhysHeapFree(req);
	return (vboxvfs_vrc2errno(vrc));
}

/*
 * Set attributes through a handle: mode and times (zero time fields are
 * left unchanged by the host).  *info is updated with what the host
 * reports afterwards.
 */
int
vboxvfs_host_setinfo(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
    SHFLFSOBJINFO *info)
{
	VBOXSFOBJINFOREQ *req;
	int vrc;

	req = VbglR0PhysHeapAlloc(sizeof(*req));
	if (req == NULL)
		return (ENOMEM);
	req->ObjInfo = *info;
	vrc = VbglR0SfHostReqSetObjInfo(vmp->vm_root, req, handle);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "SET_OBJ_INFO %#jx mode %#o: %d\n",
	    (uintmax_t)handle, info->Attr.fMode, vrc);
	if (RT_SUCCESS(vrc))
		*info = req->ObjInfo;
	VbglR0PhysHeapFree(req);
	return (vboxvfs_vrc2errno(vrc));
}

int
vboxvfs_host_volinfo(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
    SHFLVOLINFO *vi)
{
	VBOXSFVOLINFOREQ *req;
	int vrc;

	req = VbglR0PhysHeapAlloc(sizeof(*req));
	if (req == NULL)
		return (ENOMEM);
	vrc = VbglR0SfHostReqQueryVolInfo(vmp->vm_root, req, handle);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "QUERY_VOL_INFO: %d\n", vrc);
	if (RT_SUCCESS(vrc))
		*vi = req->VolInfo;
	VbglR0PhysHeapFree(req);
	return (vboxvfs_vrc2errno(vrc));
}

/*
 * Read a batch of directory entries into buf (which must come from
 * vboxvfs_host_alloc()).  *nentries is the number of SHFLDIRINFO records in
 * the buffer, *more whether the host has more.  End of directory is not an
 * error: *nentries is 0 and *more false.
 */
int
vboxvfs_host_list(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
    uint32_t lflags, void *buf, uint32_t buflen, uint32_t *nentries,
    bool *more)
{
	VBOXSFLISTDIRREQ *req;
	int vrc;

	req = VbglR0PhysHeapAlloc(sizeof(*req));
	if (req == NULL)
		return (ENOMEM);
	vrc = VbglR0SfHostReqListDir(vmp->vm_root, req, handle, NULL, lflags,
	    buf, buflen);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "LIST %#jx flags %#x: %d "
	    "entries %u more %u\n", (uintmax_t)handle, lflags, vrc,
	    req->Parms.c32Entries.u.value32, req->Parms.f32More.u.value32);
	if (RT_SUCCESS(vrc)) {
		*nentries = req->Parms.c32Entries.u.value32;
		*more = req->Parms.f32More.u.value32 != 0;
	} else if (vrc == VERR_NO_MORE_FILES) {
		*nentries = 0;
		*more = false;
		vrc = VINF_SUCCESS;
	}
	VbglR0PhysHeapFree(req);
	return (vboxvfs_vrc2errno(vrc));
}

int
vboxvfs_host_remove(struct vboxvfs_mount *vmp, const char *path,
    size_t pathlen, uint32_t rflags)
{
	VBOXSFREMOVEREQ *req;
	int vrc;

	if (pathlen >= UINT16_MAX)
		return (ENAMETOOLONG);
	req = VbglR0PhysHeapAlloc(RT_UOFFSETOF(VBOXSFREMOVEREQ,
	    StrPath.String) + pathlen + 1);
	if (req == NULL)
		return (ENOMEM);
	vboxvfs_host_setstring(&req->StrPath, path, pathlen);
	vrc = VbglR0SfHostReqRemove(vmp->vm_root, req, rflags);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "REMOVE \"%.*s\" flags %#x: %d\n",
	    (int)pathlen, path, rflags, vrc);
	VbglR0PhysHeapFree(req);
	return (vboxvfs_vrc2errno(vrc));
}

int
vboxvfs_host_rename(struct vboxvfs_mount *vmp, const char *src,
    size_t srclen, const char *dst, size_t dstlen, uint32_t rflags)
{
	VBOXSFRENAMEWITHSRCBUFREQ *req;
	SHFLSTRING *srcstr;
	int vrc;

	if (srclen >= UINT16_MAX || dstlen >= UINT16_MAX)
		return (ENAMETOOLONG);
	req = VbglR0PhysHeapAlloc(RT_UOFFSETOF(VBOXSFRENAMEWITHSRCBUFREQ,
	    StrDstPath.String) + dstlen + 1);
	if (req == NULL)
		return (ENOMEM);
	srcstr = VbglR0PhysHeapAlloc(SHFLSTRING_HEADER_SIZE + srclen + 1);
	if (srcstr == NULL) {
		VbglR0PhysHeapFree(req);
		return (ENOMEM);
	}
	vboxvfs_host_setstring(&req->StrDstPath, dst, dstlen);
	vboxvfs_host_setstring(srcstr, src, srclen);
	vrc = VbglR0SfHostReqRenameWithSrcBuf(vmp->vm_root, req, srcstr,
	    rflags);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST,
	    "RENAME \"%.*s\" -> \"%.*s\" flags %#x: %d\n", (int)srclen, src,
	    (int)dstlen, dst, rflags, vrc);
	VbglR0PhysHeapFree(srcstr);
	VbglR0PhysHeapFree(req);
	return (vboxvfs_vrc2errno(vrc));
}

/*
 * Read the target of a symbolic link into buf (kernel memory, bufsize
 * bytes); *lenp is the length without the terminating NUL.
 */
int
vboxvfs_host_readlink(struct vboxvfs_mount *vmp, const char *path,
    size_t pathlen, char *buf, size_t bufsize, size_t *lenp)
{
	char *hbuf;
	size_t len;
	int vrc;

	if (pathlen >= UINT16_MAX)
		return (ENAMETOOLONG);
	hbuf = VbglR0PhysHeapAlloc(bufsize);
	if (hbuf == NULL)
		return (ENOMEM);
	vrc = VbglR0SfHostReqReadLinkContigSimple(vmp->vm_root, path, pathlen,
	    hbuf, VbglR0PhysHeapGetPhysAddr(hbuf), bufsize);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "READLINK \"%.*s\": %d\n",
	    (int)pathlen, path, vrc);
	if (RT_SUCCESS(vrc)) {
		len = strnlen(hbuf, bufsize);
		bcopy(hbuf, buf, len);
		*lenp = len;
	}
	VbglR0PhysHeapFree(hbuf);
	return (vboxvfs_vrc2errno(vrc));
}

/*
 * Create a symbolic link; *info receives the attributes of the new link.
 */
int
vboxvfs_host_symlink(struct vboxvfs_mount *vmp, const char *path,
    size_t pathlen, const char *target, size_t targetlen,
    SHFLFSOBJINFO *info)
{
	VBOXSFCREATESYMLINKREQ *req;
	SHFLSTRING *tstr;
	int vrc;

	if (pathlen >= UINT16_MAX || targetlen >= UINT16_MAX)
		return (ENAMETOOLONG);
	req = VbglR0PhysHeapAlloc(RT_UOFFSETOF(VBOXSFCREATESYMLINKREQ,
	    StrSymlinkPath.String) + pathlen + 1);
	if (req == NULL)
		return (ENOMEM);
	tstr = VbglR0PhysHeapAlloc(SHFLSTRING_HEADER_SIZE + targetlen + 1);
	if (tstr == NULL) {
		VbglR0PhysHeapFree(req);
		return (ENOMEM);
	}
	vboxvfs_host_setstring(&req->StrSymlinkPath, path, pathlen);
	vboxvfs_host_setstring(tstr, target, targetlen);
	RT_ZERO(req->ObjInfo);
	vrc = VbglR0SfHostReqCreateSymlinkContig(vmp->vm_root, tstr,
	    VbglR0PhysHeapGetPhysAddr(tstr), req);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST,
	    "SYMLINK \"%.*s\" -> \"%.*s\": %d\n", (int)pathlen, path,
	    (int)targetlen, target, vrc);
	if (RT_SUCCESS(vrc))
		*info = req->ObjInfo;
	VbglR0PhysHeapFree(tstr);
	VbglR0PhysHeapFree(req);
	return (vboxvfs_vrc2errno(vrc));
}

/*
 * Truncate or extend a file through a handle opened for writing.
 */
int
vboxvfs_host_setsize(struct vboxvfs_mount *vmp, SHFLHANDLE handle,
    uint64_t size)
{
	int vrc;

	if (vboxvfs_sflastfn < SHFL_FN_SET_FILE_SIZE)
		return (EOPNOTSUPP);
	vrc = VbglR0SfHostReqSetFileSizeSimple(vmp->vm_root, handle, size);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "SET_FILE_SIZE %#jx %ju: %d\n",
	    (uintmax_t)handle, (uintmax_t)size, vrc);
	return (vboxvfs_vrc2errno(vrc));
}

/*
 * Data transfer.  The buffer's pages are handed to the host as a page list
 * (no copy); a buffer without pages goes through a physically contiguous
 * bounce buffer from the physical heap.  bufoff is the offset of the
 * transfer within the buffer, off the offset in the file.
 */
static int
vboxvfs_host_xfer(struct vboxvfs_mount *vmp, SHFLHANDLE handle, off_t off,
    struct buf *bp, u_int bufoff, size_t len, size_t *donep, bool write)
{
	VBOXSFREADPGLSTREQ *rreq;
	VBOXSFWRITEPGLSTREQ *wreq;
	HGCMPageListInfo *pglst;
	RTGCPHYS64 phys;
	char *bounce;
	u_int first, npages, offpage, i;
	int vrc;

	*donep = 0;
	if (len == 0)
		return (0);
	if (len > UINT32_MAX)
		return (EINVAL);
	bounce = NULL;
	offpage = bufoff & PAGE_MASK;
	npages = howmany(offpage + len, PAGE_SIZE);
	first = bufoff / PAGE_SIZE;
	/*
	 * The physical pages behind the buffer come from its kernel mapping
	 * (pmap_kextract), not from b_pages[]: struct buf has members whose
	 * presence depends on kernel options (BUF_TRACKING), so b_pages is
	 * only reliable when the module is built with the running kernel's
	 * exact option set, which a package cannot assume.  b_data is always
	 * mapped here because the mount does not advertise unmapped buffers.
	 */
	if (bp->b_data == unmapped_buf) {
		bounce = VbglR0PhysHeapAlloc(len);
		if (bounce == NULL)
			return (ENOMEM);
		if (write)
			bcopy(bp->b_data + bufoff, bounce, len);
		phys = VbglR0PhysHeapGetPhysAddr(bounce);
		offpage = phys & PAGE_MASK;
		npages = howmany(offpage + len, PAGE_SIZE);
	}
	if (write) {
		wreq = VbglR0PhysHeapAlloc(RT_UOFFSETOF_DYN(VBOXSFWRITEPGLSTREQ,
		    PgLst.aPages[npages]));
		rreq = NULL;
		pglst = wreq != NULL ? &wreq->PgLst : NULL;
	} else {
		rreq = VbglR0PhysHeapAlloc(RT_UOFFSETOF_DYN(VBOXSFREADPGLSTREQ,
		    PgLst.aPages[npages]));
		wreq = NULL;
		pglst = rreq != NULL ? &rreq->PgLst : NULL;
	}
	if (pglst == NULL) {
		vboxvfs_host_free(bounce);
		return (ENOMEM);
	}
	pglst->offFirstPage = offpage;
	if (bounce != NULL) {
		phys &= ~(RTGCPHYS64)PAGE_MASK;
		for (i = 0; i < npages; i++)
			pglst->aPages[i] = phys + (RTGCPHYS64)i * PAGE_SIZE;
	} else {
		for (i = 0; i < npages; i++) {
			phys = pmap_kextract((vm_offset_t)bp->b_data +
			    (first + i) * PAGE_SIZE);
			KASSERT(phys != 0, ("vboxvfs_host_xfer: unmapped page %u "
			    "of buffer %p", first + i, bp));
			pglst->aPages[i] = phys;
		}
	}
	if (write) {
		vrc = VbglR0SfHostReqWritePgLst(vmp->vm_root, wreq, handle, off,
		    len, npages);
		if (RT_SUCCESS(vrc))
			*donep = wreq->Parms.cb32Write.u.value32;
		VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST,
		    "WRITE %#jx off %jd len %zu pages %u: %d done %zu\n",
		    (uintmax_t)handle, (intmax_t)off, len, npages, vrc, *donep);
		VbglR0PhysHeapFree(wreq);
	} else {
		vrc = VbglR0SfHostReqReadPgLst(vmp->vm_root, rreq, handle, off,
		    len, npages);
		if (RT_SUCCESS(vrc))
			*donep = rreq->Parms.cb32Read.u.value32;
		VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST,
		    "READ %#jx off %jd len %zu pages %u: %d done %zu\n",
		    (uintmax_t)handle, (intmax_t)off, len, npages, vrc, *donep);
		VbglR0PhysHeapFree(rreq);
		if (RT_SUCCESS(vrc) && bounce != NULL)
			bcopy(bounce, bp->b_data + bufoff, *donep);
	}
	vboxvfs_host_free(bounce);
	if (RT_SUCCESS(vrc) && *donep > len)
		*donep = len;
	return (vboxvfs_vrc2errno(vrc));
}

int
vboxvfs_host_read(struct vboxvfs_mount *vmp, SHFLHANDLE handle, off_t off,
    struct buf *bp, u_int bufoff, size_t len, size_t *donep)
{

	return (vboxvfs_host_xfer(vmp, handle, off, bp, bufoff, len, donep,
	    false));
}

int
vboxvfs_host_write(struct vboxvfs_mount *vmp, SHFLHANDLE handle, off_t off,
    struct buf *bp, u_int bufoff, size_t len, size_t *donep)
{

	return (vboxvfs_host_xfer(vmp, handle, off, bp, bufoff, len, donep,
	    true));
}

int
vboxvfs_host_flush(struct vboxvfs_mount *vmp, SHFLHANDLE handle)
{
	int vrc;

	vrc = VbglR0SfHostReqFlushSimple(vmp->vm_root, handle);
	VBOXVFS_DPRINTF(VBOXVFS_DEBUG_HOST, "FLUSH %#jx: %d\n",
	    (uintmax_t)handle, vrc);
	return (vboxvfs_vrc2errno(vrc));
}
