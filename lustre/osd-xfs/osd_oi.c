// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * The FID -> object map for osd-xfs.
 *
 * On an OST, osd-ldiskfs resolves every FID through a directory hierarchy
 * rather than through the IAM b-tree (see osd_oi_lookup() in
 * lustre/osd-ldiskfs/osd_oi.c, which dispatches anything matching
 * fid_is_on_ost() straight to osd_obj_map_lookup()). The layout is:
 *
 *	O/<seq>/d<oid & (subdir_count - 1)>/<oid>
 *	O/<seq>/LAST_ID
 *
 * We reproduce it exactly, on ordinary XFS directories. Two consequences that
 * are the whole reason an OST-only XFS backend is feasible:
 *
 *   - No b-tree to port. The directory *is* the index.
 *   - No FID in the directory entry. osd_obj_add_entry() in ldiskfs passes
 *     NULL for the FID parameter, so the dirdata feature is not involved, so
 *     XFS needs no on-disk directory format change.
 *
 * The authoritative FID for an object is its trusted.lma xattr; the directory
 * name is a fast path that a scrub could rebuild. That is the same contract
 * ldiskfs offers.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/exportfs.h>
#include <linux/delayed_call.h>
#include <lustre_compat.h>
#include <lustre_compat/linux/fs.h>

#include "osd_internal.h"

/*
 * Sequence directory naming. Reserved and MDT0 sequences are rendered in
 * decimal, everything else in hex, and IDIF collapses onto sequence 0. This
 * matches osd_compat.c so a target is legible with the same mental model.
 */
static void osd_seq_name(char *buf, size_t len, u64 seq)
{
	snprintf(buf, len,
		 (fid_seq_is_rsvd(seq) || fid_seq_is_mdt0(seq)) ? "%llu" : "%llx",
		 fid_seq_is_idif(seq) ? 0ULL : seq);
}

static void osd_oid_name(char *buf, size_t len, const struct lu_fid *fid,
			 u64 id)
{
	snprintf(buf, len,
		 (fid_seq_is_rsvd(fid_seq(fid)) ||
		  fid_seq_is_mdt0(fid_seq(fid)) ||
		  fid_seq_is_idif(fid_seq(fid))) ? "%llu" : "%llx", id);
}

/*
 * Look one component up below @parent. The caller must hold the parent's
 * inode lock in the mode appropriate to what it intends to do.
 *
 * NB: lookup_noperm() rather than a path-walking lookup, because these
 * directories are internal to the OSD and must not be subject to permission
 * checks or LSM hooks. This is the same call osd-ldiskfs uses.
 */
static struct dentry *osd_lookup_locked(struct dentry *parent,
					const char *name)
{
	struct qstr qstr = QSTR_INIT(name, strlen(name));

	return lookup_noperm(&qstr, parent);
}

/* look up one component below @parent, creating it as a directory if absent */
struct dentry *osd_lookup_or_mkdir(struct osd_device *dev,
					  struct dentry *parent,
					  const char *name, bool create)
{
	struct inode *dir = d_inode(parent);
	struct dentry *dchild;

	inode_lock(dir);
	dchild = osd_lookup_locked(parent, name);
	if (IS_ERR(dchild))
		goto out;

	if (d_really_is_positive(dchild)) {
		if (!S_ISDIR(d_inode(dchild)->i_mode)) {
			dput(dchild);
			dchild = ERR_PTR(-ENOTDIR);
		}
		goto out;
	}

	if (!create) {
		dput(dchild);
		dchild = ERR_PTR(-ENOENT);
		goto out;
	}

	/*
	 * ll_vfs_mkdir() papers over the kernels where vfs_mkdir() returns an
	 * int rather than the (possibly different) resulting dentry.
	 */
	dchild = ll_vfs_mkdir(&nop_mnt_idmap, dir, dchild, 0755);
out:
	inode_unlock(dir);
	if (!IS_ERR(dchild) && d_really_is_positive(dchild))
		osd_set_nocmtime(d_inode(dchild));
	return dchild;
}

static void osd_seq_free(struct osd_obj_seq *oseq)
{
	int i;

	if (oseq->oos_dirs != NULL) {
		for (i = 0; i < oseq->oos_subdir_count; i++)
			dput(oseq->oos_dirs[i]);
		OBD_FREE_PTR_ARRAY(oseq->oos_dirs, oseq->oos_subdir_count);
	}
	dput(oseq->oos_root);
	OBD_FREE_PTR(oseq);
}

/* find, or create, the O/<seq>/ subtree */
static struct osd_obj_seq *osd_seq_load(struct osd_device *dev, u64 seq)
{
	struct osd_obj_map *map = dev->od_ost_map;
	struct osd_obj_seq *oseq;
	char name[32];
	int i, rc = 0;

	down_read(&map->om_sem);
	list_for_each_entry(oseq, &map->om_seq_list, oos_seq_list) {
		if (oseq->oos_seq == seq) {
			up_read(&map->om_sem);
			return oseq;
		}
	}
	up_read(&map->om_sem);

	down_write(&map->om_sem);
	/* somebody may have created it while we dropped the lock */
	list_for_each_entry(oseq, &map->om_seq_list, oos_seq_list) {
		if (oseq->oos_seq == seq) {
			up_write(&map->om_sem);
			return oseq;
		}
	}

	OBD_ALLOC_PTR(oseq);
	if (oseq == NULL)
		GOTO(out, rc = -ENOMEM);

	oseq->oos_seq = seq;
	oseq->oos_subdir_count = map->om_subdir_count;

	osd_seq_name(name, sizeof(name), seq);
	oseq->oos_root = osd_lookup_or_mkdir(dev, map->om_root, name, true);
	if (IS_ERR(oseq->oos_root)) {
		rc = PTR_ERR(oseq->oos_root);
		oseq->oos_root = NULL;
		OBD_FREE_PTR(oseq);
		GOTO(out, rc);
	}

	OBD_ALLOC_PTR_ARRAY(oseq->oos_dirs, oseq->oos_subdir_count);
	if (oseq->oos_dirs == NULL) {
		dput(oseq->oos_root);
		OBD_FREE_PTR(oseq);
		GOTO(out, rc = -ENOMEM);
	}

	for (i = 0; i < oseq->oos_subdir_count; i++) {
		char dname[16];

		snprintf(dname, sizeof(dname), "d%u", i);
		oseq->oos_dirs[i] = osd_lookup_or_mkdir(dev, oseq->oos_root,
							dname, true);
		if (IS_ERR(oseq->oos_dirs[i])) {
			rc = PTR_ERR(oseq->oos_dirs[i]);
			oseq->oos_dirs[i] = NULL;
			osd_seq_free(oseq);
			GOTO(out, rc);
		}
	}

	list_add(&oseq->oos_seq_list, &map->om_seq_list);
	up_write(&map->om_sem);
	return oseq;
out:
	up_write(&map->om_sem);
	return ERR_PTR(rc);
}

/*
 * Resolve a FID to the directory that holds it and the name within that
 * directory. Mirrors the ldiskfs computation, including the power-of-two mask.
 */
static int osd_fid_to_dir(const struct lu_env *env, struct osd_device *dev,
			  const struct lu_fid *fid, struct dentry **dir,
			  char *name, size_t namelen)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct ost_id *ostid = &info->oti_ostid;
	struct osd_obj_seq *oseq;
	u64 oid;
	int dirn;
	int rc;

	rc = fid_to_ostid(fid, ostid);
	if (rc != 0)
		return rc;

	oseq = osd_seq_load(dev, ostid_seq(ostid));
	if (IS_ERR(oseq))
		return PTR_ERR(oseq);

	oid = ostid_id(ostid);
	dirn = oid & (oseq->oos_subdir_count - 1);
	*dir = oseq->oos_dirs[dirn];
	osd_oid_name(name, namelen, fid, oid);

	return 0;
}

int osd_obj_map_lookup(const struct lu_env *env, struct osd_device *dev,
		       const struct lu_fid *fid, struct dentry **dentry)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct dentry *dir, *dchild;
	char *name = info->oti_name;
	int rc;

	ENTRY;

	rc = osd_fid_to_dir(env, dev, fid, &dir, name, sizeof(info->oti_name));
	if (rc != 0)
		RETURN(rc);

	inode_lock_shared(d_inode(dir));
	dchild = osd_lookup_locked(dir, name);
	inode_unlock_shared(d_inode(dir));

	if (IS_ERR(dchild))
		RETURN(PTR_ERR(dchild));

	if (d_really_is_negative(dchild)) {
		dput(dchild);
		RETURN(-ENOENT);
	}

	osd_set_nocmtime(d_inode(dchild));
	*dentry = dchild;
	RETURN(0);
}

/*
 * Create an OST object at its FID-derived path, with its identity xattrs and
 * initial attributes, in one XFS transaction. Returns the new dentry.
 */
int osd_obj_map_create(const struct lu_env *env, struct osd_device *dev,
		       const struct lu_fid *fid, umode_t mode,
		       const struct xfs_osd_attr *attr,
		       const struct xfs_osd_xattr *xattrs, unsigned int nr,
		       struct dentry **dentry)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct dentry *dir, *dchild;
	char *name = info->oti_name;
	int rc;

	ENTRY;

	rc = osd_fid_to_dir(env, dev, fid, &dir, name, sizeof(info->oti_name));
	if (rc != 0)
		RETURN(rc);

	inode_lock(d_inode(dir));
	dchild = osd_lookup_locked(dir, name);
	if (IS_ERR(dchild)) {
		rc = PTR_ERR(dchild);
		goto out;
	}
	if (d_really_is_positive(dchild)) {
		/* already there; the caller is re-creating after a crash */
		dput(dchild);
		rc = -EEXIST;
		goto out;
	}
	rc = xfs_osd_create(d_inode(dir), dchild, mode, attr, xattrs, nr);
	if (rc == 0) {
		osd_set_nocmtime(d_inode(dchild));
		*dentry = dchild;
	} else {
		dput(dchild);
	}
out:
	inode_unlock(d_inode(dir));
	RETURN(rc);
}

int osd_obj_map_delete(const struct lu_env *env, struct osd_device *dev,
		       const struct lu_fid *fid)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct dentry *dir, *dchild;
	char *name = info->oti_name;
	int rc;

	ENTRY;

	rc = osd_fid_to_dir(env, dev, fid, &dir, name, sizeof(info->oti_name));
	if (rc != 0)
		RETURN(rc);

	inode_lock(d_inode(dir));
	dchild = osd_lookup_locked(dir, name);
	if (IS_ERR(dchild)) {
		rc = PTR_ERR(dchild);
		goto out;
	}
	if (d_really_is_negative(dchild)) {
		dput(dchild);
		rc = -ENOENT;
		goto out;
	}
	rc = vfs_unlink(&nop_mnt_idmap, d_inode(dir), dchild);
	dput(dchild);
out:
	inode_unlock(d_inode(dir));
	RETURN(rc);
}

int osd_obj_map_init(const struct lu_env *env, struct osd_device *dev)
{
	struct osd_obj_map *map;

	ENTRY;

	OBD_ALLOC_PTR(map);
	if (map == NULL)
		RETURN(-ENOMEM);

	init_rwsem(&map->om_sem);
	INIT_LIST_HEAD(&map->om_seq_list);
	map->om_subdir_count = OSD_XFS_SUBDIR_COUNT;

	map->om_root = osd_lookup_or_mkdir(dev, dev->od_root_dentry, "O",
					   true);
	if (IS_ERR(map->om_root)) {
		int rc = PTR_ERR(map->om_root);

		OBD_FREE_PTR(map);
		RETURN(rc);
	}

	dev->od_ost_map = map;
	RETURN(0);
}

void osd_obj_map_fini(struct osd_device *dev)
{
	struct osd_obj_map *map = dev->od_ost_map;
	struct osd_obj_seq *oseq, *tmp;

	if (map == NULL)
		return;

	list_for_each_entry_safe(oseq, tmp, &map->om_seq_list, oos_seq_list) {
		list_del(&oseq->oos_seq_list);
		osd_seq_free(oseq);
	}
	dput(map->om_root);
	OBD_FREE_PTR(map);
	dev->od_ost_map = NULL;
}

/* ================================================================== */
/* The oi/ index, for objects whose path cannot be derived from a FID  */
/* ================================================================== */

/*
 * OST data objects live at a path computed from the FID, so they need no
 * index. Everything else does. This mirrors the classification
 * fid_is_on_ost() makes in osd-ldiskfs/osd_oi.c, minus the FLD lookup: on an
 * OST, anything that is not explicitly a local/IGIF/llog/quota FID is an OST
 * object.
 */
bool osd_fid_is_on_ost(const struct lu_fid *fid)
{
	if (fid_is_idif(fid) || fid_is_last_id(fid))
		return true;
	if (fid_is_local_file(fid) || fid_is_igif(fid) ||
	    fid_is_llog(fid) || fid_is_name_llog(fid) || fid_is_quota(fid))
		return false;
	/* an ordinary OST object sequence */
	return true;
}

/*
 * Read a symlink body into a kernel buffer.
 *
 * NB: not vfs_readlink(). That is the readlink(2) backend and copies to a
 * *user* buffer via readlink_copy(), so handing it a kernel pointer returns
 * -EFAULT. vfs_get_link() is the in-kernel accessor and hands back a pointer
 * to the link body, released by the delayed call.
 */
int osd_readlink_str(struct dentry *link, char *buf, size_t len)
{
	DEFINE_DELAYED_CALL(done);
	struct inode *inode = d_inode(link);
	const char *body;
	size_t n;

	if (!S_ISLNK(inode->i_mode))
		return -EINVAL;

	body = vfs_get_link(link, &done);
	if (IS_ERR(body))
		return PTR_ERR(body);

	n = strlen(body);
	if (n >= len) {
		do_delayed_call(&done);
		return -ENAMETOOLONG;
	}
	memcpy(buf, body, n);
	buf[n] = '\0';
	do_delayed_call(&done);

	return n;
}

static void osd_oi_name(char *buf, size_t len, const struct lu_fid *fid)
{
	snprintf(buf, len, "%llx:%x:%x", fid_seq(fid), fid_oid(fid),
		 fid_ver(fid));
}

/*
 * Turn an inode number plus generation into a dentry using the backing
 * filesystem's own NFS export operation. XFS validates the generation and
 * returns -ESTALE if the inode has been reused, which is exactly the check an
 * object index needs and is why the OI can store a bare ino:gen pair.
 */
static struct dentry *osd_dentry_by_ino_gen(struct osd_device *dev,
					    u64 ino, u32 gen)
{
	struct super_block *sb = osd_sb(dev);
	struct {
		u64 ino;
		u32 gen;
	} raw = { .ino = ino, .gen = gen };

	if (sb == NULL || sb->s_export_op == NULL ||
	    sb->s_export_op->fh_to_dentry == NULL)
		return ERR_PTR(-EOPNOTSUPP);

	/*
	 * XFS's on-wire 64-bit handle is three u32 words: u64 ino, u32 gen.
	 * Its type is FILEID_INO32_GEN | XFS_FILEID_TYPE_64FLAG (0x80).
	 * Use the full-width form even for small inode numbers. The OI already
	 * stores u64 inode numbers; narrowing here prevents large targets from
	 * resolving otherwise valid objects. fh_len excludes structure padding.
	 */
	return sb->s_export_op->fh_to_dentry(sb, (struct fid *)&raw, 3,
					     FILEID_INO32_GEN | 0x80);
}

int osd_oi_lookup(const struct lu_env *env, struct osd_device *dev,
		  const struct lu_fid *fid, struct dentry **dentry)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct osd_oi *oi = dev->od_oi;
	char *name = info->oti_name;
	char body[64];
	struct dentry *link, *d;
	u64 ino;
	u32 gen;
	int rc;

	ENTRY;

	if (oi == NULL)
		RETURN(-ENOENT);

	osd_oi_name(name, sizeof(info->oti_name), fid);

	inode_lock_shared(d_inode(oi->oi_root));
	link = osd_lookup_locked(oi->oi_root, name);
	inode_unlock_shared(d_inode(oi->oi_root));
	if (IS_ERR(link))
		RETURN(PTR_ERR(link));
	if (d_really_is_negative(link)) {
		dput(link);
		RETURN(-ENOENT);
	}

	rc = osd_readlink_str(link, body, sizeof(body));
	dput(link);
	if (rc < 0)
		RETURN(rc);

	if (sscanf(body, "%llu:%u", &ino, &gen) != 2)
		RETURN(-EFSCORRUPTED);

	d = osd_dentry_by_ino_gen(dev, ino, gen);
	if (IS_ERR(d))
		RETURN(PTR_ERR(d));

	osd_set_nocmtime(d_inode(d));
	*dentry = d;
	RETURN(0);
}

int osd_oi_insert(const struct lu_env *env, struct osd_device *dev,
		  const struct lu_fid *fid, struct inode *inode)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct osd_oi *oi = dev->od_oi;
	char *name = info->oti_name;
	char body[64];
	struct dentry *dchild;
	int rc;

	ENTRY;

	if (oi == NULL)
		RETURN(-ENOENT);

	osd_oi_name(name, sizeof(info->oti_name), fid);
	snprintf(body, sizeof(body), "%llu:%u",
		 (u64)inode->i_ino, inode->i_generation);

	inode_lock(d_inode(oi->oi_root));
	dchild = osd_lookup_locked(oi->oi_root, name);
	if (IS_ERR(dchild)) {
		rc = PTR_ERR(dchild);
		goto out;
	}
	if (d_really_is_positive(dchild)) {
		/* replace: the object was recreated with the same FID */
		rc = vfs_unlink(&nop_mnt_idmap, d_inode(oi->oi_root), dchild);
		dput(dchild);
		if (rc)
			goto out;
		dchild = osd_lookup_locked(oi->oi_root, name);
		if (IS_ERR(dchild)) {
			rc = PTR_ERR(dchild);
			goto out;
		}
	}
	rc = vfs_symlink(&nop_mnt_idmap, d_inode(oi->oi_root), dchild, body,
			 NULL);
	dput(dchild);
out:
	inode_unlock(d_inode(oi->oi_root));
	RETURN(rc);
}

int osd_oi_delete(const struct lu_env *env, struct osd_device *dev,
		  const struct lu_fid *fid)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct osd_oi *oi = dev->od_oi;
	char *name = info->oti_name;
	struct dentry *dchild;
	int rc;

	ENTRY;

	if (oi == NULL)
		RETURN(-ENOENT);

	osd_oi_name(name, sizeof(info->oti_name), fid);

	inode_lock(d_inode(oi->oi_root));
	dchild = osd_lookup_locked(oi->oi_root, name);
	if (IS_ERR(dchild)) {
		rc = PTR_ERR(dchild);
		goto out;
	}
	if (d_really_is_negative(dchild)) {
		dput(dchild);
		rc = -ENOENT;
		goto out;
	}
	rc = vfs_unlink(&nop_mnt_idmap, d_inode(oi->oi_root), dchild);
	dput(dchild);
out:
	inode_unlock(d_inode(oi->oi_root));
	RETURN(rc);
}

int osd_fid_lookup(const struct lu_env *env, struct osd_device *dev,
		   const struct lu_fid *fid, struct dentry **dentry)
{
	/*
	 * An IGIF FID *is* an (inode number, generation) pair, so it needs no
	 * index at all. These turn up for objects that exist on disk without a
	 * trusted.lma -- most importantly the CONFIGS directory, which
	 * mkfs.lustre creates in order to write CONFIGS/mountdata before
	 * Lustre has ever seen the target. Without this, local_storage looks
	 * CONFIGS up, gets an IGIF FID it cannot resolve, decides the object
	 * is missing, tries to create it, and fails with -EEXIST because the
	 * directory is right there. osd-ldiskfs resolves IGIF the same way.
	 */
	if (fid_is_igif(fid)) {
		struct dentry *d;

		d = osd_dentry_by_ino_gen(dev, lu_igif_ino(fid),
					  lu_igif_gen(fid));
		CDEBUG(D_INODE, "osd-xfs: igif "DFID" ino %llu gen %u -> %ld\n",
		       PFID(fid), (u64)lu_igif_ino(fid), lu_igif_gen(fid),
		       IS_ERR(d) ? PTR_ERR(d) : 0);
		if (IS_ERR(d))
			return PTR_ERR(d);
		osd_set_nocmtime(d_inode(d));
		*dentry = d;
		return 0;
	}

	if (osd_fid_is_on_ost(fid))
		return osd_obj_map_lookup(env, dev, fid, dentry);
	return osd_oi_lookup(env, dev, fid, dentry);
}

/* ------------------------------------------------------------------ */
/* staging: create now, bind to a name later                           */
/* ------------------------------------------------------------------ */

/*
 * Lustre creates an object (dt_create) and only afterwards inserts its name
 * into the parent index (dt_insert). Through the VFS there is no way to add a
 * second name to an existing directory, so a directory object is created in
 * oi/stage/ and renamed into place when the name arrives. Renaming does not
 * change the inode number, so the OI entry made here stays correct.
 */
int osd_stage_create(const struct lu_env *env, struct osd_device *dev,
		     const struct lu_fid *fid, umode_t mode,
		     const struct xfs_osd_attr *attr,
		     const struct xfs_osd_xattr *xattrs, unsigned int nr,
		     struct dentry **dentry)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct osd_oi *oi = dev->od_oi;
	char *name = info->oti_name;
	struct dentry *dchild;
	int rc;

	ENTRY;

	if (oi == NULL)
		RETURN(-ENOENT);

	osd_oi_name(name, sizeof(info->oti_name), fid);

	inode_lock(d_inode(oi->oi_stage));
	dchild = osd_lookup_locked(oi->oi_stage, name);
	if (IS_ERR(dchild)) {
		rc = PTR_ERR(dchild);
		goto out;
	}
	if (d_really_is_positive(dchild)) {
		dput(dchild);
		rc = -EEXIST;
		goto out;
	}

	rc = xfs_osd_create(d_inode(oi->oi_stage), dchild, mode, attr,
			    xattrs, nr);
	if (rc != 0) {
		dput(dchild);
		goto out;
	}
	*dentry = dchild;
out:
	inode_unlock(d_inode(oi->oi_stage));
	if (rc == 0) {
		osd_set_nocmtime(d_inode(*dentry));
		rc = osd_oi_insert(env, dev, fid, d_inode(*dentry));
	}
	RETURN(rc);
}

int osd_stage_bind(const struct lu_env *env, struct osd_device *dev,
		   struct dentry *parent, const char *name,
		   struct dentry *staged)
{
	struct osd_oi *oi = dev->od_oi;
	struct renamedata rd = {};
	struct dentry *target;
	struct qstr qstr = QSTR_INIT(name, strlen(name));
	int rc;

	ENTRY;

	if (oi == NULL)
		RETURN(-ENOENT);

	/* lock_rename() orders the two directory locks correctly for us */
	target = lock_rename(oi->oi_stage, parent);
	if (IS_ERR(target))
		RETURN(PTR_ERR(target));

	target = lookup_noperm(&qstr, parent);
	if (IS_ERR(target)) {
		rc = PTR_ERR(target);
		goto out;
	}
	if (d_really_is_positive(target)) {
		dput(target);
		rc = -EEXIST;
		goto out;
	}

	rd.mnt_idmap	= &nop_mnt_idmap;
	rd.old_parent	= oi->oi_stage;
	rd.old_dentry	= staged;
	rd.new_parent	= parent;
	rd.new_dentry	= target;

	rc = vfs_rename(&rd);
	dput(target);
out:
	unlock_rename(oi->oi_stage, parent);
	RETURN(rc);
}

int osd_oi_init(const struct lu_env *env, struct osd_device *dev)
{
	struct osd_oi *oi;
	int rc = 0;

	ENTRY;

	OBD_ALLOC_PTR(oi);
	if (oi == NULL)
		RETURN(-ENOMEM);

	oi->oi_root = osd_lookup_or_mkdir(dev, dev->od_root_dentry, OSD_OI_DIRNAME, true);
	if (IS_ERR(oi->oi_root)) {
		rc = PTR_ERR(oi->oi_root);
		OBD_FREE_PTR(oi);
		RETURN(rc);
	}

	oi->oi_stage = osd_lookup_or_mkdir(dev, oi->oi_root, "stage", true);
	if (IS_ERR(oi->oi_stage)) {
		rc = PTR_ERR(oi->oi_stage);
		dput(oi->oi_root);
		OBD_FREE_PTR(oi);
		RETURN(rc);
	}

	dev->od_oi = oi;
	RETURN(0);
}

void osd_oi_fini(struct osd_device *dev)
{
	struct osd_oi *oi = dev->od_oi;

	if (oi == NULL)
		return;
	dput(oi->oi_stage);
	dput(oi->oi_root);
	OBD_FREE_PTR(oi);
	dev->od_oi = NULL;
}
