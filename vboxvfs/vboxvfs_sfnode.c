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

/*
 * This file contains the sfnode implementation.  sfnodes are Shared Folder
 * nodes, which contain additional information about a given node.  These
 * are required in order to maintain the state necessary to talk to
 * VirtualBox shared folder host software.
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
 * uid and gid in sffs determine owner and group for all files.
 */
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

/*
 * Look for a cached node, if not found either handle ".." or try looking
 * via the provider. Create an entry in sfnodes if found but not cached yet.
 * If the create flag is set, a file or directory is created. If the file
 * already existed, an error is returned.
 * Nodes returned from this routine always have a vnode with its ref count
 * bumped by 1.
 */
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
