// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * LU object lifecycle and struct dt_object_operations for osd-xfs.
 *
 * Creates and attribute updates go through <linux/xfs_osd.h>, one XFS
 * transaction each; everything else uses VFS interfaces. Where an operation is
 * not implemented it returns -EOPNOTSUPP and says so: an OSD that silently
 * succeeds is far worse than one that refuses, because the failure surfaces
 * later as corruption.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/xattr.h>

#include "osd_internal.h"

/* ------------------------------------------------------------------ */
/* LMA (trusted.lma): the authoritative FID of an object              */
/* ------------------------------------------------------------------ */

int osd_lma_get(const struct lu_env *env, struct inode *inode,
		struct lu_fid *fid)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct lustre_mdt_attrs *lma = &info->oti_mdt_attrs;
	struct dentry dentry = {};
	int rc;

	dentry.d_inode = inode;
	dentry.d_sb = inode->i_sb;

	rc = __vfs_getxattr(&dentry, inode, XATTR_NAME_LMA, lma, sizeof(*lma));
	if (rc < 0)
		return rc;
	if (rc < sizeof(struct lustre_ost_attrs) &&
	    rc < sizeof(*lma))
		return -EINVAL;

	lustre_lma_swab(lma);
	if (fid != NULL)
		*fid = lma->lma_self_fid;

	return 0;
}

/* The identity xattr every object is created with, in the thread's buffer. */
void osd_lma_build(const struct lu_env *env, const struct lu_fid *fid,
		   struct xfs_osd_xattr *xattr)
{
	struct lustre_mdt_attrs *lma = &osd_oti_get(env)->oti_mdt_attrs;

	memset(lma, 0, sizeof(*lma));
	lustre_lma_init(lma, fid, LMAC_FID_ON_OST, 0);
	lustre_lma_swab(lma);
	xattr->name = XATTR_NAME_LMA;
	xattr->value = lma;
	xattr->size = sizeof(*lma);
}

/* ------------------------------------------------------------------ */
/* LU object lifecycle                                                 */
/* ------------------------------------------------------------------ */

static void osd_object_init0(struct osd_object *obj)
{
	struct inode *inode = obj->oo_inode;

	if (inode == NULL)
		return;

	obj->oo_dt.do_body_ops = &osd_body_ops;
	obj->oo_dt.do_lu.lo_header->loh_attr |= LOHA_EXISTS;
	obj->oo_dt.do_lu.lo_header->loh_attr |=
		(inode->i_mode & S_IFMT);
	/*
	 * Deliberately do NOT set do_index_ops here.
	 *
	 * Every index object is a directory on disk in this OSD -- a name
	 * index and a key/value index look identical at the inode level -- so
	 * stamping osd_dir_ops on any directory pre-empts do_index_try() and
	 * mis-routes key/value indexes. That is what made nodemap_idx_insert_batch
	 * reach osd_dir_insert() and fault reading an opaque record as a
	 * struct lu_fid pointer. do_index_try() is the only thing that knows
	 * which flavour the caller wants.
	 */
	osd_set_nocmtime(inode);
}

static int osd_lu_object_init(const struct lu_env *env, struct lu_object *l,
			      const struct lu_object_conf *conf)
{
	struct osd_object *obj = osd_obj(l);
	struct osd_device *dev = osd_dev(l->lo_dev);
	const struct lu_fid *fid = lu_object_fid(l);
	struct dentry *dentry = NULL;
	int rc;

	ENTRY;

	LASSERT(obj->oo_inode == NULL);

	/*
	 * Give the object a declare-only body vector up front. Lustre may
	 * declare a write before the object exists, and dt_declare_record_write()
	 * asserts on a NULL do_body_ops. osd_object_init0() upgrades it to the
	 * full vector once there is an inode.
	 */
	obj->oo_dt.do_body_ops = &osd_body_ops_new;

	/* the backing filesystem root has a well-known FID */
	if (lu_fid_eq(fid, &dev->od_root_fid)) {
		obj->oo_inode = igrab(d_inode(dev->od_root_dentry));
		obj->oo_dentry = dget(dev->od_root_dentry);
		osd_object_init0(obj);
		RETURN(0);
	}

	rc = osd_fid_lookup(env, dev, fid, &dentry);
	CDEBUG(D_INODE, "osd-xfs: object_init "DFID" -> rc %d\n",
	       PFID(fid), rc);
	if (rc == -ENOENT) {
		/*
		 * Not an error: Lustre routinely instantiates a dt_object for
		 * a FID before creating it. LOHA_EXISTS stays clear.
		 */
		RETURN(0);
	}
	if (rc != 0)
		RETURN(rc);

	obj->oo_dentry = dentry;
	obj->oo_inode = igrab(d_inode(dentry));

	/* Recovery may change size: complete it before exposing record reads. */
	if (osd_rec_logged(obj)) {
		rc = osd_rec_recover(obj);
		if (rc)
			RETURN(rc);
	}
	if (osd_rec_small(obj)) {
		rc = osd_rec_small_load(env, obj);
		if (rc == -ENOENT) {
			/* A crash leftover, now removed: the object is absent. */
			dput(obj->oo_dentry);
			obj->oo_dentry = NULL;
			iput(obj->oo_inode);
			obj->oo_inode = NULL;
			RETURN(0);
		}
		if (rc)
			RETURN(rc);
	}

	osd_object_init0(obj);

	RETURN(0);
}

static void osd_lu_object_delete(const struct lu_env *env, struct lu_object *l)
{
	struct osd_object *obj = osd_obj(l);

	if (obj->oo_dentry != NULL) {
		dput(obj->oo_dentry);
		obj->oo_dentry = NULL;
	}
	if (obj->oo_inode != NULL) {
		iput(obj->oo_inode);
		obj->oo_inode = NULL;
	}
}

static void osd_lu_object_free(const struct lu_env *env, struct lu_object *l)
{
	struct osd_object *obj = osd_obj(l);

	dt_object_fini(&obj->oo_dt);
	OBD_SLAB_FREE_PTR(obj, osd_xfs_object_cachep());
}

static void osd_lu_object_release(const struct lu_env *env,
				  struct lu_object *l)
{
}

static int osd_lu_object_print(const struct lu_env *env, void *cookie,
			       lu_printer_t p, const struct lu_object *l)
{
	struct osd_object *o = osd_obj(l);

	return (*p)(env, cookie, LUSTRE_OSD_XFS_NAME "-object@%p(i:%p:%lu)",
		    o, o->oo_inode,
		    o->oo_inode ? o->oo_inode->i_ino : 0UL);
}

const struct lu_object_operations osd_lu_obj_ops = {
	.loo_object_init	= osd_lu_object_init,
	.loo_object_delete	= osd_lu_object_delete,
	.loo_object_release	= osd_lu_object_release,
	.loo_object_free	= osd_lu_object_free,
	.loo_object_print	= osd_lu_object_print,
};

struct lu_object *osd_object_alloc(const struct lu_env *env,
				   const struct lu_object_header *hdr,
				   struct lu_device *d)
{
	struct osd_object *mo;

	struct lu_object *l;

	OBD_SLAB_ALLOC_PTR_GFP(mo, osd_xfs_object_cachep(), GFP_NOFS);
	if (mo == NULL)
		return NULL;

	l = &mo->oo_dt.do_lu;
	dt_object_init(&mo->oo_dt, NULL, d);
	mo->oo_dt.do_ops = &osd_obj_ops;
	l->lo_ops = &osd_lu_obj_ops;
	init_rwsem(&mo->oo_dt.dd_sem);
	init_rwsem(&mo->oo_guard);
	init_rwsem(&mo->oo_ext_guard);
	mutex_init(&mo->oo_rec_lock);

	return l;
}

/* ------------------------------------------------------------------ */
/* attributes                                                          */
/* ------------------------------------------------------------------ */

static void osd_inode_getattr(const struct lu_env *env, struct inode *inode,
			      struct dentry *dentry, struct lu_attr *attr)
{
	attr->la_valid |= LA_ATIME | LA_MTIME | LA_CTIME | LA_MODE |
			  LA_SIZE | LA_BLOCKS | LA_UID | LA_GID |
			  LA_FLAGS | LA_NLINK | LA_RDEV | LA_BLKSIZE |
			  LA_TYPE | LA_BTIME;

	attr->la_atime  = inode_get_atime_sec(inode);
	attr->la_mtime  = inode_get_mtime_sec(inode);
	attr->la_ctime  = inode_get_ctime_sec(inode);
	attr->la_btime  = 0;
	attr->la_mode   = inode->i_mode;
	attr->la_size   = i_size_read(inode);
	/*
	 * NOT inode->i_blocks. XFS never maintains that field for the VFS: it
	 * computes the block count in xfs_vn_getattr() from i_nblocks plus
	 * i_delayed_blks and writes it straight into the kstat. Reading
	 * i_blocks therefore reports 0 for every object however much data it
	 * holds, which reaches the client as st_blocks == 0 and makes du, and
	 * anything that bills by allocation, wrong.
	 *
	 * ->getattr() is the only way to get the real figure, and
	 * xfs_vn_getattr() takes the inode from path->dentry, so a dentry is
	 * required rather than optional. Ask for STATX_BLOCKS alone with
	 * query_flags 0: this runs on every attribute fetch, and
	 * AT_STATX_FORCE_SYNC here would mean a writeback per stat.
	 */
	attr->la_blocks = inode->i_blocks;
	if (dentry != NULL && inode->i_op != NULL &&
	    inode->i_op->getattr != NULL) {
		struct path p = { .mnt = NULL, .dentry = dentry };
		struct kstat st = {};

		if (inode->i_op->getattr(&nop_mnt_idmap, &p, &st,
					 STATX_BLOCKS, 0) == 0)
			attr->la_blocks = st.blocks;
	}
	attr->la_uid    = i_uid_read(inode);
	attr->la_gid    = i_gid_read(inode);
	attr->la_flags  = 0;
	attr->la_nlink  = inode->i_nlink;
	attr->la_rdev   = inode->i_rdev;
	attr->la_blksize = 1 << inode->i_blkbits;
	attr->la_blkbits = inode->i_blkbits;
}

static int osd_attr_get(const struct lu_env *env, struct dt_object *dt,
			struct lu_attr *attr)
{
	struct osd_object *obj = osd_dt_obj(dt);

	if (unlikely(obj->oo_destroyed))
		return -ENOENT;

	if (obj->oo_inode == NULL)
		return -ENOENT;

	down_read(&obj->oo_guard);
	osd_inode_getattr(env, obj->oo_inode, obj->oo_dentry, attr);
	up_read(&obj->oo_guard);
	/*
	 * A small record's body is its attribute, not file data. Callers
	 * branch on the size (ofd_seq_load() treats 0 as "never written"),
	 * so report the current record length.
	 */
	if (osd_rec_small(obj)) {
		mutex_lock(&obj->oo_rec_lock);
		attr->la_size = obj->oo_rec_len;
		mutex_unlock(&obj->oo_rec_lock);
	}

	return 0;
}

static int osd_declare_attr_set(const struct lu_env *env, struct dt_object *dt,
				const struct lu_attr *attr,
				struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct inode *inode = obj->oo_inode;
	__u64 blocks = OSD_DECL_BLK_ATTR_SET;

	/*
	 * A size increase is the only attribute change that can consume
	 * space, so it is the only one costed beyond the flat charge.
	 *
	 * Both pointers really can be NULL. The inode may not exist yet,
	 * because Lustre declares an attr_set alongside the create that
	 * precedes it, and there is then no size to grow from. And @attr
	 * itself is NULL from the llog code, which declares an attr_set in
	 * five places without saying what it will change; dt_declare_attr_set()
	 * passes that straight through. Dereferencing it oopsed the OSS during
	 * mount.
	 */
	if (inode != NULL && attr != NULL && (attr->la_valid & LA_SIZE)) {
		__u64 size = i_size_read(inode);

		if (attr->la_size > size)
			blocks += (attr->la_size - size +
				   i_blocksize(inode) - 1) >> inode->i_blkbits;
	}

	osd_declare_op(th, OSD_OT_ATTR_SET, blocks, 0);

	return 0;
}

/*
 * Translate the Lustre attributes this backend applies into one XFS update.
 *
 * Lustre's la_mode carries the file type in S_IFMT; dt_find_or_create()
 * passes an attr describing a regular file even when dof_type is DFT_INDEX.
 * Only the permission bits are taken, as osd-ldiskfs does
 * (osd_handler.c:3283), so an index directory stays a directory.
 *
 * LA_NLINK is deliberately ignored. The VFS owns i_nlink (see osd_ref_add):
 * a caller that passes la_nlink = 0 for an object being torn down would zero
 * the count while the name is still present, and the following unlink then
 * underflows it, which XFS answers by shutting the filesystem down.
 */
static int osd_attr_to_xfs(const struct lu_attr *attr, struct xfs_osd_attr *xa)
{
	memset(xa, 0, sizeof(*xa));
	if (attr->la_valid & LA_MODE) {
		xa->valid |= XFS_OSD_SET_MODE;
		xa->mode = attr->la_mode & S_IALLUGO;
	}
	if (attr->la_valid & LA_UID) {
		xa->valid |= XFS_OSD_SET_UID;
		xa->uid = make_kuid(&init_user_ns, attr->la_uid);
		if (!uid_valid(xa->uid))
			return -EINVAL;
	}
	if (attr->la_valid & LA_GID) {
		xa->valid |= XFS_OSD_SET_GID;
		xa->gid = make_kgid(&init_user_ns, attr->la_gid);
		if (!gid_valid(xa->gid))
			return -EINVAL;
	}
	if (attr->la_valid & LA_ATIME) {
		xa->valid |= XFS_OSD_SET_ATIME;
		xa->atime = (struct timespec64){ .tv_sec = attr->la_atime };
	}
	if (attr->la_valid & LA_MTIME) {
		xa->valid |= XFS_OSD_SET_MTIME;
		xa->mtime = (struct timespec64){ .tv_sec = attr->la_mtime };
	}
	if (attr->la_valid & LA_CTIME) {
		xa->valid |= XFS_OSD_SET_CTIME;
		xa->ctime = (struct timespec64){ .tv_sec = attr->la_ctime };
	}
	return 0;
}

/*
 * One logged XFS transaction for mode, owner and times.
 *
 * mark_inode_dirty() is not enough on XFS: it has no ->dirty_inode or
 * ->write_inode, so fields written straight into the VFS inode reached disk
 * only if some later transaction happened to log that inode.
 */
static int osd_attr_set(const struct lu_env *env, struct dt_object *dt,
			const struct lu_attr *attr, struct thandle *handle)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct inode *inode = obj->oo_inode;
	struct xfs_osd_attr xa;
	int rc;

	if (inode == NULL)
		return -ENOENT;

	osd_exec_op(env, handle, OSD_OT_ATTR_SET, 0, 0);
	if ((attr->la_valid & LA_SIZE) && osd_rec_logged(obj)) {
		rc = osd_rec_resize(obj, attr->la_size);
		if (rc)
			return rc;
	}

	rc = osd_attr_to_xfs(attr, &xa);
	if (rc)
		return rc;
	if (xa.valid) {
		down_write(&obj->oo_guard);
		inode_lock(inode);
		rc = xfs_osd_setattr(obj->oo_dentry, &xa);
		inode_unlock(inode);
		up_write(&obj->oo_guard);
		if (rc) {
			CDEBUG(D_INODE, "%s: attr_set "DFID" valid %#llx: rc = %d\n",
			       osd_name(osd_obj2dev(obj)),
			       PFID(lu_object_fid(&dt->do_lu)), attr->la_valid,
			       rc);
			return rc;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* xattrs                                                              */
/* ------------------------------------------------------------------ */

static int osd_xattr_get(const struct lu_env *env, struct dt_object *dt,
			 struct lu_buf *buf, const char *name)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct inode *inode = obj->oo_inode;
	struct dentry dentry = {};
	int rc;

	if (inode == NULL)
		return -ENOENT;

	dentry.d_inode = inode;
	dentry.d_sb = inode->i_sb;

	down_read(&obj->oo_ext_guard);
	rc = __vfs_getxattr(&dentry, inode, name,
			    buf->lb_buf, buf->lb_buf ? buf->lb_len : 0);
	up_read(&obj->oo_ext_guard);

	return rc;
}

/*
 * Costed for a remote attribute block. A value that fits in the inode's
 * literal area needs no block at all, but which case this is depends on what
 * else the inode already carries, and that cannot be known here.
 */
static int osd_declare_xattr_set(const struct lu_env *env,
				 struct dt_object *dt,
				 const struct lu_attr *attr,
				 const struct lu_buf *buf, const char *name,
				 int fl, struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	unsigned int bits = obj->oo_inode ? obj->oo_inode->i_blkbits :
		osd_sb(osd_obj2dev(obj))->s_blocksize_bits;
	u64 bytes = buf ? buf->lb_len : XATTR_SIZE_MAX;

	if (bytes > XATTR_SIZE_MAX)
		return -E2BIG;
	osd_declare_op(th, OSD_OT_XATTR_SET,
		       OSD_DECL_BLK_XATTR_SET + DIV_ROUND_UP_ULL(bytes, 1U << bits), 0);

	return 0;
}

static int osd_xattr_set(const struct lu_env *env, struct dt_object *dt,
			 const struct lu_buf *buf, const char *name, int fl,
			 struct thandle *handle)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct inode *inode = obj->oo_inode;
	struct dentry dentry = {};
	int flags = 0;
	int rc;

	if (inode == NULL) {
		rc = -ENOENT;
		goto failed;
	}

	osd_exec_op(env, handle, OSD_OT_XATTR_SET, 0, 0);

	if (fl & LU_XATTR_CREATE)
		flags |= XATTR_CREATE;
	if (fl & LU_XATTR_REPLACE)
		flags |= XATTR_REPLACE;

	dentry.d_inode = inode;
	dentry.d_sb = inode->i_sb;

	down_write(&obj->oo_ext_guard);
	inode_lock(inode);
	rc = __vfs_setxattr(&nop_mnt_idmap, &dentry, inode, name,
			    buf->lb_buf, buf->lb_len, flags);
	inode_unlock(inode);
	up_write(&obj->oo_ext_guard);

failed:
	/* Version-set callers discard return values; preserve failure in th. */
	if (rc && handle) {
		handle->th_result = osd_commit_error(osd_obj2dev(obj), rc);
		return handle->th_result;
	}
	return rc;
}

/*
 * Zero cost: a removal only frees space. It is still declared, so that the
 * execute side is not counted as having run without one.
 */
static int osd_declare_xattr_del(const struct lu_env *env,
				 struct dt_object *dt, const char *name,
				 struct thandle *th)
{
	osd_declare_op(th, OSD_OT_XATTR_DEL, 0, 0);

	return 0;
}

static int osd_xattr_del(const struct lu_env *env, struct dt_object *dt,
			 const char *name, struct thandle *handle)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct inode *inode = obj->oo_inode;
	struct dentry dentry = {};
	int rc;

	if (inode == NULL)
		return -ENOENT;

	osd_exec_op(env, handle, OSD_OT_XATTR_DEL, 0, 0);

	dentry.d_inode = inode;
	dentry.d_sb = inode->i_sb;

	down_write(&obj->oo_ext_guard);
	inode_lock(inode);
	rc = __vfs_removexattr(&nop_mnt_idmap, &dentry, name);
	inode_unlock(inode);
	up_write(&obj->oo_ext_guard);

	return rc;
}

static int osd_xattr_list(const struct lu_env *env, struct dt_object *dt,
			  const struct lu_buf *buf)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct inode *inode = obj->oo_inode;
	struct dentry dentry = {};
	int rc;

	if (inode == NULL)
		return -ENOENT;

	dentry.d_inode = inode;
	dentry.d_sb = inode->i_sb;

	down_read(&obj->oo_ext_guard);
	rc = vfs_listxattr(&dentry, buf->lb_buf, buf->lb_len);
	up_read(&obj->oo_ext_guard);

	return rc;
}

/* ------------------------------------------------------------------ */
/* create / destroy                                                    */
/* ------------------------------------------------------------------ */

static void osd_ah_init(const struct lu_env *env, struct dt_allocation_hint *ah,
			struct dt_object *parent, struct dt_object *child,
			umode_t child_mode)
{
	memset(ah, 0, sizeof(*ah));
	ah->dah_parent = parent;
}

static int osd_declare_create(const struct lu_env *env, struct dt_object *dt,
			      struct lu_attr *attr,
			      struct dt_allocation_hint *hint,
			      struct dt_object_format *dof,
			      struct thandle *th)
{
	const struct lu_fid *fid = lu_object_fid(&dt->do_lu);

	osd_declare_op(th, OSD_OT_CREATE, OSD_DECL_BLK_CREATE,
		       OSD_DECL_INO_CREATE + !osd_fid_is_on_ost(fid));

	return 0;
}

static int osd_create(const struct lu_env *env, struct dt_object *dt,
		      struct lu_attr *attr, struct dt_allocation_hint *hint,
		      struct dt_object_format *dof, struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_device *osd = osd_obj2dev(obj);
	const struct lu_fid *fid = lu_object_fid(&dt->do_lu);
	struct dentry *dentry = NULL;
	struct xfs_osd_xattr lma;
	struct xfs_osd_attr xa;
	umode_t mode;
	int rc;

	ENTRY;

	if (dt_object_exists(dt))
		RETURN(-EEXIST);

	osd_exec_op(env, th, OSD_OT_CREATE, 0, 0);

	switch (dof->dof_type) {
	case DFT_REGULAR:
	case DFT_SYM:
	case DFT_NODE:
		mode = S_IFREG | 0666;
		break;
	case DFT_DIR:
	case DFT_INDEX:
		/*
		 * Both are XFS directories. A DFT_INDEX with variable-length
		 * string keys and FID records is exactly a directory; an index
		 * with fixed-size binary keys is not, and osd_index_try()
		 * refuses those.
		 */
		mode = S_IFDIR | 0755;
		break;
	default:
		CERROR("%s: " OSD_XFS_NAME " cannot create object type %d "
		       "(FID "DFID")\n", osd_name(osd), dof->dof_type,
		       PFID(fid));
		RETURN(-EOPNOTSUPP);
	}

	if (fid_is_igif(fid)) {
		CERROR("%s: refusing to create an IGIF object "DFID
		       ": IGIF names an inode that must already exist\n",
		       osd_name(osd), PFID(fid));
		RETURN(-EPERM);
	}

	/*
	 * The inode, its name, trusted.lma and the initial mode, owner and
	 * times are one XFS transaction, so a crash leaves all of them or
	 * none: never a named inode without an identity.
	 */
	memset(&xa, 0, sizeof(xa));
	if (attr != NULL) {
		rc = osd_attr_to_xfs(attr, &xa);
		if (rc)
			RETURN(rc);
	}
	osd_lma_build(env, fid, &lma);

	if (osd_fid_is_on_ost(fid)) {
		/* path is derived from the FID; create it in place */
		if (S_ISDIR(mode)) {
			CERROR("%s: refusing to create a directory in the O/ "
			       "tree ("DFID")\n", osd_name(osd), PFID(fid));
			RETURN(-EOPNOTSUPP);
		}
		rc = osd_obj_map_create(env, osd, fid, mode, &xa, &lma, 1,
					&dentry);
	} else {
		/*
		 * No name yet: Lustre supplies it later via dt_insert(). Create
		 * the object in oi/stage/ and record it in the OI, so it is
		 * reachable by FID immediately; dt_insert() renames it into
		 * place, which does not change its inode number.
		 */
		rc = osd_stage_create(env, osd, fid, mode, &xa, &lma, 1,
				      &dentry);
	}
	if (rc != 0)
		RETURN(rc);

	obj->oo_dentry = dentry;
	obj->oo_inode = igrab(d_inode(dentry));
	if (osd_rec_small(obj)) {
		/* No value yet: reads see empty until the first write. */
		obj->oo_rec_cached = true;
		obj->oo_rec_len = 0;
	}


	osd_object_init0(obj);

	RETURN(0);
}

static int osd_declare_destroy(const struct lu_env *env, struct dt_object *dt,
			       struct thandle *th)
{
	osd_declare_op(th, OSD_OT_DESTROY, 0, 0);

	return 0;
}

/*
 * Destroy must be idempotent with respect to the name.
 *
 * Lustre tears a named object down as dt_delete(parent, name) followed by
 * dt_destroy(object). The first call already removed the link through the
 * parent index; this object still holds a positive dentry for it, so
 * unconditionally unlinking again drives i_nlink to -1. XFS detects that
 * ("Inode ... link count dropped below zero"), cancels the transaction and
 * shuts the filesystem down -- after which every operation returns -EIO and
 * the target cannot be mounted again.
 *
 * So only remove the name if it is still there. An object that was created
 * but never bound to a name (it is still sitting in oi/stage/) is the case
 * that genuinely needs the unlink here.
 */
static int osd_destroy(const struct lu_env *env, struct dt_object *dt,
		       struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_device *osd = osd_obj2dev(obj);
	const struct lu_fid *fid = lu_object_fid(&dt->do_lu);
	int rc = 0;

	ENTRY;

	if (!dt_object_exists(dt) || obj->oo_destroyed)
		RETURN(-ENOENT);

	osd_exec_op(env, th, OSD_OT_DESTROY, 0, 0);

	if (osd_fid_is_on_ost(fid)) {
		/*
		 * An OST data object has exactly one name, in the O/ tree, and
		 * no parent index that Lustre would delete from.
		 */
		rc = osd_obj_map_delete(env, osd, fid);
		if (rc == -ENOENT)
			rc = 0;
	} else {
		struct dentry *dentry = obj->oo_dentry;

		if (dentry != NULL && d_really_is_positive(dentry) &&
		    !d_unhashed(dentry) && dentry->d_parent != NULL &&
		    d_inode(dentry)->i_nlink > 0) {
			struct dentry *parent = dget_parent(dentry);

			inode_lock(d_inode(parent));
			/* re-check under the parent lock */
			if (d_really_is_positive(dentry) &&
			    d_inode(dentry)->i_nlink > 0) {
				if (S_ISDIR(d_inode(dentry)->i_mode)) {
					rc = vfs_rmdir(&nop_mnt_idmap,
						       d_inode(parent),
						       dentry, NULL);
					if (rc == -ENOTEMPTY) {
						inode_unlock(d_inode(parent));
						rc = osd_purge_index_dir(osd,
									 dentry);
						inode_lock(d_inode(parent));
						if (rc == 0)
							rc = vfs_rmdir(
							    &nop_mnt_idmap,
							    d_inode(parent),
							    dentry, NULL);
					}
				}
				else
					rc = vfs_unlink(&nop_mnt_idmap,
							d_inode(parent),
							dentry);
			}
			inode_unlock(d_inode(parent));
			dput(parent);

			if (rc == -ENOENT)
				rc = 0;
		}

		if (rc == 0)
			rc = osd_oi_delete(env, osd, fid);
		if (rc == -ENOENT)
			rc = 0;
	}

	if (rc == 0) {
		obj->oo_destroyed = 1;
		dt->do_lu.lo_header->loh_attr &= ~LOHA_EXISTS;
	}

	RETURN(rc);
}

/*
 * Zero cost: i_nlink lives in the inode, and the name that the link count is
 * being adjusted for is charged by the index operation that adds or removes
 * it.
 */
static int osd_declare_ref_add(const struct lu_env *env, struct dt_object *dt,
			       struct thandle *th)
{
	osd_declare_op(th, OSD_OT_REF_ADD, 0, 0);

	return 0;
}

static int osd_ref_add(const struct lu_env *env, struct dt_object *dt,
		       struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);

	if (obj->oo_inode == NULL)
		return -ENOENT;

	osd_exec_op(env, th, OSD_OT_REF_ADD, 0, 0);

	return 0;
}

static int osd_declare_ref_del(const struct lu_env *env, struct dt_object *dt,
			       struct thandle *th)
{
	osd_declare_op(th, OSD_OT_REF_DEL, 0, 0);

	return 0;
}

static int osd_ref_del(const struct lu_env *env, struct dt_object *dt,
		       struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);

	if (obj->oo_inode == NULL)
		return -ENOENT;

	osd_exec_op(env, th, OSD_OT_REF_DEL, 0, 0);

	return 0;
}

/*
 * sync_inode_metadata() does nothing on XFS, which has no ->write_inode:
 * metadata is durable when the log is. fsync writes the range back and
 * forces the inode's log sequence.
 */
static int osd_object_sync(const struct lu_env *env, struct dt_object *dt,
			   __u64 start, __u64 end)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct file *file;
	int rc;

	if (obj->oo_inode == NULL || obj->oo_dentry == NULL)
		return -ENOENT;
	file = dentry_open(&(struct path){ .mnt = osd_obj2dev(obj)->od_mnt,
					   .dentry = obj->oo_dentry },
			   O_RDONLY | O_NOATIME | O_LARGEFILE, current_cred());
	if (IS_ERR(file))
		return PTR_ERR(file);
	rc = vfs_fsync_range(file, start, end == OBD_OBJECT_EOF ? LLONG_MAX :
			     end, 0);
	fput(file);
	return rc;
}

static int osd_index_try(const struct lu_env *env, struct dt_object *dt,
			 const struct dt_index_features *feat)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_device *osd = osd_obj2dev(obj);

	/*
	 * Only dt_directory_features is a name -> FID map whose record is a
	 * struct dt_insert_rec. Everything else is a generic key/value index
	 * whose record is an opaque blob, and handing one of those to
	 * osd_dir_ops makes it read the first eight bytes of the record as a
	 * struct lu_fid pointer. That is a general protection fault, which is
	 * exactly what happened when this routed on DT_IND_VARKEY instead:
	 * llog_process_th oopsed dereferencing 0x746c7561666564, the ASCII
	 * "default". osd-ldiskfs makes the same split -- directory features to
	 * the name index, everything else to IAM.
	 *
	 * NB: also called during the *declare* phase, before the object
	 * exists, so a NULL inode must be accepted.
	 */
	/*
	 * The object-table pseudo index. OFD starts layout LFSCK after
	 * recovery and ofd_prepare() fails the mount if this is refused, so it
	 * is not optional for an OST. See osd_otable.c.
	 */
	if (feat == &dt_otable_features) {
		dt->do_index_ops = &osd_otable_ops;
		return 0;
	}

	if (feat == &dt_directory_features) {
		if (obj->oo_inode != NULL && !S_ISDIR(obj->oo_inode->i_mode))
			return -ENOTDIR;
		dt->do_index_ops = &osd_dir_ops;
		return 0;
	}

	/*
	 * Generic key/value: FLD, nodemap, the llog name indexes.
	 * Served by osd_kv_ops, one symlink per entry named hex(key) with
	 * hex(record) as the body. See osd_index.c.
	 *
	 * A variable key (DT_IND_VARKEY) is a NUL-terminated string bounded by
	 * dif_keysize_max; ldiskfs bounds its lvar containers the same way.
	 */
	if (feat->dif_keysize_max > 0 && feat->dif_recsize_max > 0 &&
	    feat->dif_keysize_max <= OSD_KV_KEY_MAX &&
	    feat->dif_recsize_max <= OSD_KV_REC_MAX) {
		if (obj->oo_inode != NULL && !S_ISDIR(obj->oo_inode->i_mode))
			return -ENOTDIR;
		obj->oo_keysize = feat->dif_keysize_max;
		obj->oo_recsize = feat->dif_recsize_max;
		obj->oo_varkey = !!(feat->dif_flags & DT_IND_VARKEY);
		dt->do_index_ops = &osd_kv_ops;
		CDEBUG(D_INODE,
		       "%s: index "DFID" as key/value: key %zu%s rec %zu\n",
		       osd_name(osd), PFID(lu_object_fid(&dt->do_lu)),
		       feat->dif_keysize_max, obj->oo_varkey ? " (var)" : "",
		       feat->dif_recsize_max);
		return 0;
	}

	CWARN("%s: " OSD_XFS_NAME " cannot serve index shape for "DFID
	      ": flags %x key %zu..%zu rec %zu..%zu\n",
	      osd_name(osd), PFID(lu_object_fid(&dt->do_lu)), feat->dif_flags,
	      feat->dif_keysize_min, feat->dif_keysize_max,
	      feat->dif_recsize_min, feat->dif_recsize_max);
	return -EOPNOTSUPP;
}

const struct dt_object_operations osd_obj_ops = {
	.do_attr_get		= osd_attr_get,
	.do_declare_attr_set	= osd_declare_attr_set,
	.do_attr_set		= osd_attr_set,
	.do_xattr_get		= osd_xattr_get,
	.do_declare_xattr_set	= osd_declare_xattr_set,
	.do_xattr_set		= osd_xattr_set,
	.do_declare_xattr_del	= osd_declare_xattr_del,
	.do_xattr_del		= osd_xattr_del,
	.do_xattr_list		= osd_xattr_list,
	.do_ah_init		= osd_ah_init,
	.do_declare_create	= osd_declare_create,
	.do_create		= osd_create,
	.do_declare_destroy	= osd_declare_destroy,
	.do_destroy		= osd_destroy,
	.do_declare_ref_add	= osd_declare_ref_add,
	.do_ref_add		= osd_ref_add,
	.do_declare_ref_del	= osd_declare_ref_del,
	.do_ref_del		= osd_ref_del,
	.do_index_try		= osd_index_try,
	.do_object_sync		= osd_object_sync,
};
