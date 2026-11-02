// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * struct dt_index_operations for osd-xfs: name -> FID indexes backed by
 * ordinary XFS directories.
 *
 * These are not optional for an OST. obdclass/local_storage.c builds every
 * internal named object of a target -- CONFIGS, last_rcvd, reply_data, the
 * llog catalogs -- by creating an object and then inserting its name into a
 * parent directory index, and obdclass/llog_osd.c does the same for named
 * llogs. Without index operations, mounting an OST fails at
 * local_storage.c:332 with -ENOTDIR from dt_try_as_dir().
 *
 * A key difference from osd-ldiskfs: ldiskfs stores the child FID inside the
 * directory entry (the "dirdata" feature). XFS has no such extension and
 * adding one would mean changing the XFS on-disk directory format, which this
 * port explicitly does not do. Instead a lookup reads the child's trusted.lma
 * xattr to get its FID. That costs one extra xattr read per lookup -- but
 * because we formatted with -i size=1024 the LMA is in the inode's shortform
 * attribute fork, so it is already in the inode buffer and costs no extra I/O.
 * This is the trade the design study identified, and it is why an OST-only
 * backend does not need FID-in-dirent.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/exportfs.h>
#include <linux/hex.h>

#include <lustre_compat.h>
#include <lustre_compat/linux/fs.h>

#include "osd_internal.h"

/* ------------------------------------------------------------------ */
/* point operations                                                    */
/* ------------------------------------------------------------------ */

static int osd_dir_lookup(const struct lu_env *env, struct dt_object *dt,
			  struct dt_rec *rec, const struct dt_key *key)
{
	struct osd_object *obj = osd_dt_obj(dt);
	const char *name = (const char *)key;
	struct qstr qstr = QSTR_INIT(name, strlen(name));
	struct lu_fid *fid = (struct lu_fid *)rec;
	struct dentry *dchild;
	int rc;

	ENTRY;

	if (obj->oo_dentry == NULL || obj->oo_inode == NULL)
		RETURN(-ENOENT);
	if (!S_ISDIR(obj->oo_inode->i_mode))
		RETURN(-ENOTDIR);

	/* "." and ".." are asked for by local_storage when it builds a dir */
	if (strcmp(name, ".") == 0) {
		*fid = *lu_object_fid(&dt->do_lu);
		RETURN(1);
	}

	inode_lock_shared(obj->oo_inode);
	dchild = lookup_noperm(&qstr, obj->oo_dentry);
	inode_unlock_shared(obj->oo_inode);

	if (IS_ERR(dchild)) {
		CDEBUG(D_INODE, "osd-xfs: lookup '%s' in "DFID" failed: rc = %ld\n",
		       name, PFID(lu_object_fid(&dt->do_lu)), PTR_ERR(dchild));
		RETURN(PTR_ERR(dchild));
	}
	if (d_really_is_negative(dchild)) {
		CDEBUG(D_INODE, "osd-xfs: lookup '%s' in "DFID": ENOENT\n",
		       name, PFID(lu_object_fid(&dt->do_lu)));
		dput(dchild);
		RETURN(-ENOENT);
	}

	rc = osd_lma_get(env, d_inode(dchild), fid);
	CDEBUG(D_INODE, "osd-xfs: lookup '%s' in "DFID" -> ino %lu lma rc %d\n",
	       name, PFID(lu_object_fid(&dt->do_lu)),
	       d_inode(dchild)->i_ino, rc);
	if (rc == -ENODATA) {
		/*
		 * No LMA. Objects created outside Lustre (or the backing
		 * filesystem's own lost+found) look like this. IGIF gives them
		 * a stable identity derived from ino/generation, which is what
		 * ldiskfs does for pre-2.0 inodes.
		 */
		lu_igif_build(fid, d_inode(dchild)->i_ino,
			      d_inode(dchild)->i_generation);
		rc = 0;
	}
	dput(dchild);

	/*
	 * dt_lookup() inverts the return convention: it maps >0 to 0 ("found")
	 * and 0 to -ENOENT ("not found"). Returning 0 here for a successful
	 * lookup therefore told the caller the entry did not exist, which sent
	 * local_storage off to re-create CONFIGS and fail with -EEXIST.
	 * osd-ldiskfs returns 1 for the same reason.
	 */
	RETURN(rc == 0 ? 1 : rc);
}

static int osd_dir_insert(const struct lu_env *env, struct dt_object *dt,
			  const struct dt_rec *rec, const struct dt_key *key,
			  struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_device *osd = osd_obj2dev(obj);
	const struct dt_insert_rec *irec = (const struct dt_insert_rec *)rec;
	const char *name = (const char *)key;
	const struct lu_fid *cfid = irec->rec_fid;
	struct dentry *child = NULL;
	int rc;

	ENTRY;

	if (obj->oo_dentry == NULL || obj->oo_inode == NULL)
		RETURN(-ENOENT);
	if (!S_ISDIR(obj->oo_inode->i_mode))
		RETURN(-ENOTDIR);
	/*
	 * Guard against a key/value index being routed here: its record is an
	 * opaque blob, and reading rec_fid out of it dereferences garbage.
	 */
	if (obj->oo_keysize != 0) {
		CERROR("%s: name-index insert on a key/value index "DFID"\n",
		       osd_name(osd), PFID(lu_object_fid(&dt->do_lu)));
		RETURN(-EINVAL);
	}

	/*
	 * local_storage inserts "." and ".." explicitly because ldiskfs
	 * directories carry them as real entries. XFS maintains both itself,
	 * so accept and ignore rather than fail the caller.
	 */
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		RETURN(0);

	osd_trans_mutated(th);

	rc = osd_fid_lookup(env, osd, cfid, &child);
	if (rc != 0) {
		CDEBUG(D_INODE,
		       "%s: cannot find child "DFID" to insert as '%s': rc = %d\n",
		       osd_name(osd), PFID(cfid), name, rc);
		RETURN(rc);
	}

	/*
	 * If the child is still in oi/stage/ it has no name yet: bind it here
	 * by renaming it into place. Otherwise it already lives somewhere and
	 * we are adding another name, which only works for non-directories.
	 */
	CDEBUG(D_INODE, "osd-xfs: insert '%s' -> "DFID" into "DFID
	       " (staged=%d)\n", name, PFID(cfid),
	       PFID(lu_object_fid(&dt->do_lu)),
	       child->d_parent == osd->od_oi->oi_stage);
	if (child->d_parent == osd->od_oi->oi_stage) {
		rc = osd_stage_bind(env, osd, obj->oo_dentry, name, child);
	} else if (S_ISDIR(d_inode(child)->i_mode)) {
		CERROR("%s: refusing a second name '%s' for directory "DFID
		       ": a directory cannot be hard linked\n",
		       osd_name(osd), name, PFID(cfid));
		rc = -EPERM;
	} else {
		struct qstr qstr = QSTR_INIT(name, strlen(name));
		struct dentry *target;

		inode_lock(obj->oo_inode);
		target = lookup_noperm(&qstr, obj->oo_dentry);
		if (IS_ERR(target)) {
			rc = PTR_ERR(target);
		} else if (d_really_is_positive(target)) {
			dput(target);
			rc = -EEXIST;
		} else {
			rc = vfs_link(child, &nop_mnt_idmap, obj->oo_inode,
				      target, NULL);
			dput(target);
		}
		inode_unlock(obj->oo_inode);
	}

	dput(child);
	RETURN(rc);
}


/*
 * Empty a directory so it can be removed.
 *
 * Lustre deletes the *name* of an index and destroys the object as two steps
 * (local_object_unlink: dt_delete, dt_ref_del, dt_destroy), and it does not
 * empty the index first -- with an IAM index there is nothing to empty,
 * because the entries live inside a regular file. Here an index is a
 * directory, so vfs_rmdir() returns -ENOTEMPTY and the mount fails:
 * nodemap_cache_find_create() hit exactly this when it discarded a stale
 * nodemap cache.
 *
 * Removing the contents is therefore the correct reading of the caller's
 * intent, but only because a directory name is deleted only when the object
 * behind it is being destroyed. The depth limit keeps an unexpected shape from
 * turning a bug into recursive deletion.
 */
int osd_purge_dir(struct osd_device *osd, struct dentry *dir, int depth)
{
	struct osd_dir_reader rd;
	struct osd_dir_ent *ent;
	int rc;

	if (depth > 2) {
		CERROR("%s: refusing to purge '%pd' at depth %d\n",
		       osd_name(osd), dir, depth);
		return -ELOOP;
	}

	rc = osd_dir_reader_init(&rd, osd, dir);
	if (rc != 0)
		return rc == -ENOENT ? 0 : rc;

	for (;;) {
		struct qstr qstr;
		struct dentry *child;

		rc = osd_dir_reader_next(&rd, &ent);
		if (rc != 0) {
			if (rc == 1)
				rc = 0;
			break;
		}

		qstr.name = ent->de_name;
		qstr.len = ent->de_namelen;

		inode_lock(d_inode(dir));
		child = lookup_noperm(&qstr, dir);
		if (IS_ERR(child)) {
			rc = PTR_ERR(child);
			inode_unlock(d_inode(dir));
			break;
		}
		if (d_really_is_negative(child)) {
			dput(child);
			inode_unlock(d_inode(dir));
			continue;
		}

		if (S_ISDIR(d_inode(child)->i_mode)) {
			inode_unlock(d_inode(dir));
			rc = osd_purge_dir(osd, child, depth + 1);
			if (rc == 0) {
				inode_lock(d_inode(dir));
				rc = vfs_rmdir(&nop_mnt_idmap, d_inode(dir),
					       child, NULL);
				inode_unlock(d_inode(dir));
			}
		} else {
			rc = vfs_unlink(&nop_mnt_idmap, d_inode(dir), child);
			inode_unlock(d_inode(dir));
		}
		dput(child);

		if (rc == -ENOENT)
			rc = 0;
		if (rc != 0)
			break;

		/* entries shifted under us; restart the scan */
		osd_dir_reader_seek(&rd, 0);
	}

	osd_dir_reader_fini(&rd);
	return rc;
}

static int osd_dir_delete(const struct lu_env *env, struct dt_object *dt,
			  const struct dt_key *key, struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	const char *name = (const char *)key;
	struct qstr qstr = QSTR_INIT(name, strlen(name));
	struct dentry *dchild;
	int rc;

	ENTRY;

	if (obj->oo_dentry == NULL || obj->oo_inode == NULL)
		RETURN(-ENOENT);
	if (!S_ISDIR(obj->oo_inode->i_mode))
		RETURN(-ENOTDIR);

	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		RETURN(0);

	osd_trans_mutated(th);

	inode_lock(obj->oo_inode);
	dchild = lookup_noperm(&qstr, obj->oo_dentry);
	if (IS_ERR(dchild)) {
		rc = PTR_ERR(dchild);
		goto out;
	}
	if (d_really_is_negative(dchild)) {
		dput(dchild);
		rc = -ENOENT;
		goto out;
	}
	if (S_ISDIR(d_inode(dchild)->i_mode)) {
		rc = vfs_rmdir(&nop_mnt_idmap, obj->oo_inode, dchild, NULL);
		if (rc == -ENOTEMPTY) {
			/* an index being discarded; see osd_purge_dir() */
			inode_unlock(obj->oo_inode);
			rc = osd_purge_dir(osd_obj2dev(obj), dchild, 0);
			inode_lock(obj->oo_inode);
			if (rc == 0)
				rc = vfs_rmdir(&nop_mnt_idmap, obj->oo_inode,
					       dchild, NULL);
		}
	} else {
		rc = vfs_unlink(&nop_mnt_idmap, obj->oo_inode, dchild);
	}
	dput(dchild);
out:
	inode_unlock(obj->oo_inode);
	RETURN(rc);
}


/* ------------------------------------------------------------------ */
/* batched directory reader                                            */
/* ------------------------------------------------------------------ */

struct osd_dr_ctx {
	struct dir_context	 ctx;
	struct osd_dir_reader	*r;
};

static bool osd_dr_filldir(struct dir_context *ctx, const char *name,
			   int namelen, loff_t offset, u64 ino,
			   unsigned int d_type)
{
	struct osd_dr_ctx *dc = container_of(ctx, struct osd_dr_ctx, ctx);
	struct osd_dir_reader *r = dc->r;
	struct osd_dir_ent *e;

	/*
	 * No room: return false WITHOUT storing. That is the contract -- false
	 * means "not taken", and the filesystem leaves its position on this
	 * entry so the next iterate_dir() call re-emits it. Storing and then
	 * returning false would duplicate the entry; storing and returning
	 * false for *every* entry (the first version of this code) makes the
	 * iterator never advance at all.
	 */
	if (r->dr_nents >= OSD_DIR_BATCH)
		return false;

	if (namelen > NAME_MAX)
		return true;			/* skip, keep going */

	/* "." and ".." are maintained by the filesystem, not by Lustre */
	if ((namelen == 1 && name[0] == '.') ||
	    (namelen == 2 && name[0] == '.' && name[1] == '.'))
		return true;

	e = &r->dr_ents[r->dr_nents++];
	memcpy(e->de_name, name, namelen);
	e->de_name[namelen] = '\0';
	e->de_namelen = namelen;
	e->de_ino = ino;
	e->de_type = d_type;
	e->de_off = offset;

	return true;
}

int osd_dir_reader_init(struct osd_dir_reader *r, struct osd_device *osd,
			struct dentry *dentry)
{
	struct file *f;

	memset(r, 0, sizeof(*r));

	if (dentry == NULL || d_really_is_negative(dentry))
		return -ENOENT;

	OBD_ALLOC_PTR_ARRAY_LARGE(r->dr_ents, OSD_DIR_BATCH);
	if (r->dr_ents == NULL)
		return -ENOMEM;

	f = dentry_open(&(struct path){ .mnt = osd->od_mnt, .dentry = dentry },
			O_RDONLY | O_DIRECTORY | O_LARGEFILE, current_cred());
	if (IS_ERR(f)) {
		OBD_FREE_PTR_ARRAY_LARGE(r->dr_ents, OSD_DIR_BATCH);
		r->dr_ents = NULL;
		return PTR_ERR(f);
	}

	r->dr_file = f;
	return 0;
}

void osd_dir_reader_fini(struct osd_dir_reader *r)
{
	if (r->dr_file != NULL) {
		fput(r->dr_file);
		r->dr_file = NULL;
	}
	if (r->dr_ents != NULL) {
		OBD_FREE_PTR_ARRAY_LARGE(r->dr_ents, OSD_DIR_BATCH);
		r->dr_ents = NULL;
	}
}

void osd_dir_reader_seek(struct osd_dir_reader *r, loff_t pos)
{
	if (r->dr_file != NULL)
		r->dr_file->f_pos = pos;
	r->dr_nents = 0;
	r->dr_cur = 0;
}

int osd_dir_reader_next(struct osd_dir_reader *r, struct osd_dir_ent **ent)
{
	struct osd_dr_ctx dc = {
		.ctx.actor = osd_dr_filldir,
		.r	   = r,
	};
	int rc;

	if (r->dr_file == NULL)
		return 1;

	if (r->dr_cur < r->dr_nents) {
		*ent = &r->dr_ents[r->dr_cur++];
		return 0;
	}

	/* refill */
	r->dr_nents = 0;
	r->dr_cur = 0;
	dc.ctx.pos = r->dr_file->f_pos;

	rc = iterate_dir(r->dr_file, &dc.ctx);
	if (rc < 0)
		return rc;

	if (r->dr_nents == 0)
		return 1;			/* end of directory */

	*ent = &r->dr_ents[r->dr_cur++];
	return 0;
}

/* ------------------------------------------------------------------ */
/* iterator                                                            */
/* ------------------------------------------------------------------ */

/*
 * Directory iterator state. The heavy lifting is in the batched reader above;
 * this only tracks which entry we are on and its cookie.
 */
struct osd_it_dir {
	struct osd_object	*oid_obj;
	struct osd_dir_reader	 oid_rd;
	struct osd_dir_ent	*oid_ent;
	bool			 oid_valid;
	/* decoded binary key, for the key/value index flavour */
	u8			 oid_keybuf[OSD_KV_KEY_MAX];
	int			 oid_keylen;
};

static struct dt_it *osd_it_dir_init(const struct lu_env *env,
				     struct dt_object *dt, __u32 attr)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_device *osd = osd_obj2dev(obj);
	struct osd_it_dir *it;
	int rc;

	ENTRY;

	if (obj->oo_dentry == NULL || obj->oo_inode == NULL)
		RETURN(ERR_PTR(-ENOENT));
	if (!S_ISDIR(obj->oo_inode->i_mode))
		RETURN(ERR_PTR(-ENOTDIR));

	OBD_ALLOC_PTR(it);
	if (it == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	rc = osd_dir_reader_init(&it->oid_rd, osd, obj->oo_dentry);
	if (rc != 0) {
		OBD_FREE_PTR(it);
		RETURN(ERR_PTR(rc));
	}

	it->oid_obj = obj;
	lu_object_get(&dt->do_lu);

	RETURN((struct dt_it *)it);
}

static void osd_it_dir_fini(const struct lu_env *env, struct dt_it *di)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;

	osd_dir_reader_fini(&it->oid_rd);
	if (it->oid_obj != NULL)
		lu_object_put(env, &it->oid_obj->oo_dt.do_lu);
	OBD_FREE_PTR(it);
}

/* 0 = positioned on an entry, 1 = end, <0 = error */
static int osd_it_dir_advance(struct osd_it_dir *it)
{
	int rc;

	it->oid_valid = false;
	rc = osd_dir_reader_next(&it->oid_rd, &it->oid_ent);
	if (rc == 0)
		it->oid_valid = true;
	return rc;
}

static int osd_it_dir_get(const struct lu_env *env, struct dt_it *di,
			  const struct dt_key *key)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;

	/* only "start from the beginning" is used by the current callers */
	osd_dir_reader_seek(&it->oid_rd, 0);
	it->oid_valid = false;
	return 0;
}

static void osd_it_dir_put(const struct lu_env *env, struct dt_it *di)
{
}

static int osd_it_dir_next(const struct lu_env *env, struct dt_it *di)
{
	return osd_it_dir_advance((struct osd_it_dir *)di);
}

static struct dt_key *osd_it_dir_key(const struct lu_env *env,
				     const struct dt_it *di)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;

	if (!it->oid_valid)
		return ERR_PTR(-ENOENT);
	return (struct dt_key *)it->oid_ent->de_name;
}

static int osd_it_dir_key_size(const struct lu_env *env,
			       const struct dt_it *di)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;

	if (!it->oid_valid)
		return -ENOENT;
	return it->oid_ent->de_namelen;
}

static int osd_it_dir_rec(const struct lu_env *env, const struct dt_it *di,
			  struct dt_rec *rec, __u32 attr)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;
	struct osd_object *obj = it->oid_obj;
	struct lu_dirent *ent = (struct lu_dirent *)rec;
	struct qstr qstr;
	struct dentry *dchild;
	struct lu_fid fid;
	int namelen;
	int rc;

	if (!it->oid_valid)
		return -ENOENT;

	namelen = it->oid_ent->de_namelen;
	qstr.name = it->oid_ent->de_name;
	qstr.len = namelen;

	/*
	 * No FID in the XFS directory entry, so read the child's LMA. Because
	 * the target is formatted with -i size=1024 that xattr is in the
	 * inode's shortform attribute fork and costs no extra I/O.
	 */
	inode_lock_shared(obj->oo_inode);
	dchild = lookup_noperm(&qstr, obj->oo_dentry);
	inode_unlock_shared(obj->oo_inode);
	if (IS_ERR(dchild))
		return PTR_ERR(dchild);
	if (d_really_is_negative(dchild)) {
		dput(dchild);
		return -ENOENT;
	}

	rc = osd_lma_get(env, d_inode(dchild), &fid);
	if (rc == -ENODATA) {
		lu_igif_build(&fid, d_inode(dchild)->i_ino,
			      d_inode(dchild)->i_generation);
		rc = 0;
	}
	dput(dchild);
	if (rc != 0)
		return rc;

	memset(ent, 0, sizeof(*ent));
	fid_cpu_to_le(&ent->lde_fid, &fid);
	ent->lde_hash = cpu_to_le64(it->oid_ent->de_off);
	ent->lde_namelen = cpu_to_le16(namelen);
	ent->lde_attrs = cpu_to_le32(LUDA_FID);
	memcpy(ent->lde_name, it->oid_ent->de_name, namelen);
	ent->lde_reclen = cpu_to_le16(lu_dirent_calc_size(namelen, LUDA_FID));

	return 0;
}

static __u64 osd_it_dir_store(const struct lu_env *env,
			      const struct dt_it *di)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;

	if (!it->oid_valid)
		return 0;
	return it->oid_ent->de_off;
}

/*
 * Position the iterator at @hash.
 *
 * The return convention is not the same as ->next():
 *   >0  positioned on a valid record
 *    0  not positioned; the caller is expected to call ->next()
 *   <0  error
 *
 * Returning ->next()'s "1 means end of index" straight through says
 * "positioned on a record" for an empty index, and the caller then calls
 * ->rec() and gets -ENOENT. That is what made fld_index_init() fail with -2 on
 * a freshly formatted target, which failed the whole mount.
 */
static int osd_it_dir_load(const struct lu_env *env, const struct dt_it *di,
			   __u64 hash)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;
	int rc;

	osd_dir_reader_seek(&it->oid_rd, hash);
	it->oid_valid = false;

	rc = osd_it_dir_advance(it);
	if (rc == 0)
		return 1;		/* landed on a record */
	if (rc == 1)
		return 0;		/* nothing there */
	return rc;
}

const struct dt_index_operations osd_dir_ops = {
	.dio_lookup		= osd_dir_lookup,
	.dio_insert		= osd_dir_insert,
	.dio_delete		= osd_dir_delete,
	.dio_it			= {
		.init		= osd_it_dir_init,
		.fini		= osd_it_dir_fini,
		.get		= osd_it_dir_get,
		.put		= osd_it_dir_put,
		.next		= osd_it_dir_next,
		.key		= osd_it_dir_key,
		.key_size	= osd_it_dir_key_size,
		.rec		= osd_it_dir_rec,
		.store		= osd_it_dir_store,
		.load		= osd_it_dir_load,
	}
};

/* ================================================================== */
/* Fixed-size binary key/value index                                   */
/* ================================================================== */

/*
 * FLD and nodemap are indexes with fixed-size binary keys and
 * records, not names. An XFS directory cannot store a binary key, so each
 * entry is a symlink whose *name* is the hex-encoded key and whose *body* is
 * the hex-encoded record.
 *
 * Why a symlink rather than a regular file: the record is at most 32 bytes
 * (the largest is union nodemap_rec), which hex-encodes to 64 characters
 * and lives inline in the inode. A regular file would cost a data block and a
 * separate read per lookup.
 *
 * Why not a b-tree: these indexes hold a handful of entries on an OST -- one
 * FLD range per sequence, a few nodemaps. XFS directories are already b-trees,
 * so lookup is O(log n) on the name. If a later index turns out to be large
 * this is the piece to revisit; it is deliberately self-contained.
 *
 * Keys are compared byte-wise by the directory, which orders hex strings the
 * same way as the underlying big-endian byte strings. Lustre stores FLD keys
 * big-endian, so directory order matches key order for them. Callers that need
 * ordered iteration of a little-endian key would not get it; none of the
 * current users do (FLD loads everything into an in-memory cache at mount).
 */

static void osd_hex_encode(char *dst, const void *src, size_t len)
{
	static const char h[] = "0123456789abcdef";
	const u8 *s = src;
	size_t i;

	for (i = 0; i < len; i++) {
		dst[i * 2]     = h[s[i] >> 4];
		dst[i * 2 + 1] = h[s[i] & 0xf];
	}
	dst[len * 2] = '\0';
}

static int osd_hex_decode(void *dst, const char *src, size_t len)
{
	u8 *d = dst;
	size_t i;

	for (i = 0; i < len; i++) {
		int hi = hex_to_bin(src[i * 2]);
		int lo = hex_to_bin(src[i * 2 + 1]);

		if (hi < 0 || lo < 0)
			return -EFSCORRUPTED;
		d[i] = (hi << 4) | lo;
	}
	return 0;
}

static int osd_kv_sizes(struct osd_object *obj, size_t *ks, size_t *rs)
{
	if (obj->oo_keysize == 0 || obj->oo_recsize == 0)
		return -EINVAL;
	if (obj->oo_keysize > OSD_KV_KEY_MAX ||
	    obj->oo_recsize > OSD_KV_REC_MAX)
		return -E2BIG;
	*ks = obj->oo_keysize;
	*rs = obj->oo_recsize;
	return 0;
}

/*
 * Length of a key as presented by the caller.
 *
 * A DT_IND_VARKEY index uses NUL-terminated strings bounded by
 * dif_keysize_max (llog names, config names). Everything else uses the full
 * fixed width. ldiskfs makes the same distinction when it chooses between an
 * lvar and an lfix IAM container.
 */
static size_t osd_kv_keylen(struct osd_object *obj, const struct dt_key *key,
			    size_t ks)
{
	if (!obj->oo_varkey)
		return ks;
	return strnlen((const char *)key, ks);
}

static int osd_kv_lookup(const struct lu_env *env, struct dt_object *dt,
			 struct dt_rec *rec, const struct dt_key *key)
{
	struct osd_object *obj = osd_dt_obj(dt);
	char name[OSD_KV_KEY_MAX * 2 + 1];
	char body[OSD_KV_REC_MAX * 2 + 1];
	struct qstr qstr;
	struct dentry *dchild;
	size_t ks, rs;
	int rc;

	ENTRY;

	if (obj->oo_dentry == NULL || obj->oo_inode == NULL)
		RETURN(-ENOENT);
	rc = osd_kv_sizes(obj, &ks, &rs);
	if (rc)
		RETURN(rc);

	ks = osd_kv_keylen(obj, key, ks);
	osd_hex_encode(name, key, ks);
	qstr.name = name;
	qstr.len = ks * 2;

	inode_lock_shared(obj->oo_inode);
	dchild = lookup_noperm(&qstr, obj->oo_dentry);
	inode_unlock_shared(obj->oo_inode);
	if (IS_ERR(dchild))
		RETURN(PTR_ERR(dchild));
	if (d_really_is_negative(dchild)) {
		dput(dchild);
		RETURN(0);		/* dt_lookup() maps 0 to -ENOENT */
	}

	rc = osd_readlink_str(dchild, body, sizeof(body));
	dput(dchild);
	if (rc < 0)
		RETURN(rc);
	if (rc != (int)(rs * 2))
		RETURN(-EFSCORRUPTED);

	rc = osd_hex_decode(rec, body, rs);
	if (rc)
		RETURN(rc);

	RETURN(1);			/* found */
}

static int osd_kv_insert(const struct lu_env *env, struct dt_object *dt,
			 const struct dt_rec *rec, const struct dt_key *key,
			 struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	char name[OSD_KV_KEY_MAX * 2 + 1];
	char body[OSD_KV_REC_MAX * 2 + 1];
	struct qstr qstr;
	struct dentry *dchild;
	size_t ks, rs;
	int rc;

	ENTRY;

	if (obj->oo_dentry == NULL || obj->oo_inode == NULL)
		RETURN(-ENOENT);
	rc = osd_kv_sizes(obj, &ks, &rs);
	if (rc)
		RETURN(rc);

	ks = osd_kv_keylen(obj, key, ks);
	osd_hex_encode(name, key, ks);
	osd_hex_encode(body, rec, rs);

	osd_trans_mutated(th);

	qstr.name = name;
	qstr.len = ks * 2;

	inode_lock(obj->oo_inode);
	dchild = lookup_noperm(&qstr, obj->oo_dentry);
	if (IS_ERR(dchild)) {
		rc = PTR_ERR(dchild);
		goto out;
	}
	if (d_really_is_positive(dchild)) {
		/* an index insert of an existing key is an update */
		rc = vfs_unlink(&nop_mnt_idmap, obj->oo_inode, dchild);
		dput(dchild);
		if (rc)
			goto out;
		dchild = lookup_noperm(&qstr, obj->oo_dentry);
		if (IS_ERR(dchild)) {
			rc = PTR_ERR(dchild);
			goto out;
		}
	}
	rc = vfs_symlink(&nop_mnt_idmap, obj->oo_inode, dchild, body, NULL);
	dput(dchild);
out:
	inode_unlock(obj->oo_inode);
	RETURN(rc);
}

static int osd_kv_delete(const struct lu_env *env, struct dt_object *dt,
			 const struct dt_key *key, struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	char name[OSD_KV_KEY_MAX * 2 + 1];
	struct qstr qstr;
	struct dentry *dchild;
	size_t ks, rs;
	int rc;

	ENTRY;

	if (obj->oo_dentry == NULL || obj->oo_inode == NULL)
		RETURN(-ENOENT);
	rc = osd_kv_sizes(obj, &ks, &rs);
	if (rc)
		RETURN(rc);

	ks = osd_kv_keylen(obj, key, ks);
	osd_hex_encode(name, key, ks);
	qstr.name = name;
	qstr.len = ks * 2;

	osd_trans_mutated(th);

	inode_lock(obj->oo_inode);
	dchild = lookup_noperm(&qstr, obj->oo_dentry);
	if (IS_ERR(dchild)) {
		rc = PTR_ERR(dchild);
		goto out;
	}
	if (d_really_is_negative(dchild)) {
		dput(dchild);
		rc = -ENOENT;
		goto out;
	}
	rc = vfs_unlink(&nop_mnt_idmap, obj->oo_inode, dchild);
	dput(dchild);
out:
	inode_unlock(obj->oo_inode);
	RETURN(rc);
}

/* iterator: reuse the directory walker, decode key/record per entry */

static struct dt_it *osd_it_kv_init(const struct lu_env *env,
				    struct dt_object *dt, __u32 attr)
{
	return osd_it_dir_init(env, dt, attr);
}

static struct dt_key *osd_it_kv_key(const struct lu_env *env,
				    const struct dt_it *di)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;
	struct osd_object *obj = it->oid_obj;
	size_t ks, rs;

	if (!it->oid_valid)
		return ERR_PTR(-ENOENT);
	if (osd_kv_sizes(obj, &ks, &rs))
		return ERR_PTR(-EINVAL);
	if (it->oid_ent->de_namelen & 1)
		return ERR_PTR(-EFSCORRUPTED);
	ks = it->oid_ent->de_namelen / 2;
	if (ks > obj->oo_keysize)
		return ERR_PTR(-EFSCORRUPTED);
	memset(it->oid_keybuf, 0, sizeof(it->oid_keybuf));
	if (osd_hex_decode(it->oid_keybuf, it->oid_ent->de_name, ks))
		return ERR_PTR(-EFSCORRUPTED);
	it->oid_keylen = ks;

	return (struct dt_key *)it->oid_keybuf;
}

static int osd_it_kv_key_size(const struct lu_env *env, const struct dt_it *di)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;

	if (it->oid_obj->oo_varkey)
		return it->oid_keylen ?: it->oid_ent->de_namelen / 2;
	return it->oid_obj->oo_keysize;
}

static int osd_it_kv_rec(const struct lu_env *env, const struct dt_it *di,
			 struct dt_rec *rec, __u32 attr)
{
	struct osd_it_dir *it = (struct osd_it_dir *)di;
	struct osd_object *obj = it->oid_obj;
	char body[OSD_KV_REC_MAX * 2 + 1];
	struct qstr qstr;
	struct dentry *dchild;
	size_t ks, rs;
	int rc;

	if (!it->oid_valid)
		return -ENOENT;
	rc = osd_kv_sizes(obj, &ks, &rs);
	if (rc)
		return rc;

	qstr.name = it->oid_ent->de_name;
	qstr.len = it->oid_ent->de_namelen;

	inode_lock_shared(obj->oo_inode);
	dchild = lookup_noperm(&qstr, obj->oo_dentry);
	inode_unlock_shared(obj->oo_inode);
	if (IS_ERR(dchild))
		return PTR_ERR(dchild);
	if (d_really_is_negative(dchild)) {
		dput(dchild);
		return -ENOENT;
	}

	rc = osd_readlink_str(dchild, body, sizeof(body));
	dput(dchild);
	if (rc < 0)
		return rc;
	if (rc != (int)(rs * 2))
		return -EFSCORRUPTED;

	return osd_hex_decode(rec, body, rs);
}

static int osd_it_kv_load(const struct lu_env *env, const struct dt_it *di,
			  __u64 hash)
{
	return osd_it_dir_load(env, di, hash);
}

const struct dt_index_operations osd_kv_ops = {
	.dio_lookup		= osd_kv_lookup,
	.dio_insert		= osd_kv_insert,
	.dio_delete		= osd_kv_delete,
	.dio_it			= {
		.init		= osd_it_kv_init,
		.fini		= osd_it_dir_fini,
		.get		= osd_it_dir_get,
		.put		= osd_it_dir_put,
		.next		= osd_it_dir_next,
		.key		= osd_it_kv_key,
		.key_size	= osd_it_kv_key_size,
		.rec		= osd_it_kv_rec,
		.store		= osd_it_dir_store,
		.load		= osd_it_kv_load,
	}
};
