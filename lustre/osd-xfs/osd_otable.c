// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * The object table iterator for osd-xfs.
 *
 * LFSCK asks the OSD for a pseudo-index on the FID
 * {FID_SEQ_LOCAL_FILE, OTABLE_IT_OID, 0} and walks it to visit every object on
 * the target. OFD starts layout LFSCK automatically after recovery
 * (ofd_obd.c:541), and ofd_prepare() fails the whole mount if lfsck_register()
 * cannot get this iterator -- so an OST cannot come up without one.
 *
 * osd-ldiskfs implements this by walking the ext4 inode table directly. There
 * is no equivalent available to a module on XFS, and bulkstat is not exported,
 * so this walks the namespace instead:
 *
 *   phase 0: O/<seq>/d<N>/<oid>   -- every OST data object
 *   phase 1: oi/<seq>:<oid>:<ver> -- every other object, by definition, since
 *                                    the OI is what makes them findable
 *
 * That is a complete enumeration of what this OSD creates. It will not find an
 * object that exists on disk but is absent from both -- which is exactly the
 * corruption a scrub is supposed to detect -- so this is an honest iterator
 * for the layout LFSCK's purposes but is NOT a substitute for an OI scrub.
 * Reconstructing the OI from a full device scan needs bulkstat and belongs
 * with the kernel-side work.
 *
 * The position cookie is an ordinal. Restarting from a checkpoint re-walks and
 * skips, which is O(n) but happens rarely; the alternative, an inode-number
 * cookie like ldiskfs uses, needs an inode-ordered scan we do not have.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/namei.h>

#include <lustre_compat.h>
#include <lustre_compat/linux/fs.h>

#include "osd_internal.h"

struct osd_otable_it {
	struct osd_device	*ooi_dev;
	struct osd_object	*ooi_obj;

	int			 ooi_phase;	/* 0 = O/ tree, 1 = oi/, 2 = end */

	/* phase 0 cursors */
	struct osd_dir_reader	 ooi_leaf;	/* O/<seq>/dN */
	bool			 ooi_leaf_open;
	struct osd_obj_seq	*ooi_seq;	/* current sequence */
	int			 ooi_dirn;	/* current dN index */

	/* phase 1 cursor */
	struct osd_dir_reader	 ooi_oi;
	bool			 ooi_oi_open;

	u64			 ooi_ordinal;
	u64			 ooi_skip;	/* entries to skip after load() */
	struct lu_fid		 ooi_fid;
	bool			 ooi_valid;
	bool			 ooi_ready;
};

static void osd_ot_close_leaf(struct osd_otable_it *it)
{
	if (it->ooi_leaf_open) {
		osd_dir_reader_fini(&it->ooi_leaf);
		it->ooi_leaf_open = false;
	}
}

static void osd_ot_close_oi(struct osd_otable_it *it)
{
	if (it->ooi_oi_open) {
		osd_dir_reader_fini(&it->ooi_oi);
		it->ooi_oi_open = false;
	}
}

/* ------------------------------------------------------------------ */
/* the walk                                                            */
/* ------------------------------------------------------------------ */

/* move to the next (sequence, dN) pair that has an open leaf directory */
static int osd_ot_next_leaf(struct osd_otable_it *it)
{
	struct osd_device *osd = it->ooi_dev;
	struct osd_obj_map *map = osd->od_ost_map;
	int rc;

	osd_ot_close_leaf(it);

	for (;;) {
		if (it->ooi_seq == NULL) {
			down_read(&map->om_sem);
			it->ooi_seq = list_first_entry_or_null(&map->om_seq_list,
							       struct osd_obj_seq,
							       oos_seq_list);
			up_read(&map->om_sem);
			it->ooi_dirn = 0;
			if (it->ooi_seq == NULL)
				return 1;	/* no sequences at all */
		}

		if (it->ooi_dirn >= it->ooi_seq->oos_subdir_count) {
			down_read(&map->om_sem);
			if (list_is_last(&it->ooi_seq->oos_seq_list,
					 &map->om_seq_list)) {
				it->ooi_seq = NULL;
				up_read(&map->om_sem);
				return 1;	/* done with the O/ tree */
			}
			it->ooi_seq = list_next_entry(it->ooi_seq,
						      oos_seq_list);
			up_read(&map->om_sem);
			it->ooi_dirn = 0;
			continue;
		}

		rc = osd_dir_reader_init(&it->ooi_leaf, osd,
					 it->ooi_seq->oos_dirs[it->ooi_dirn]);
		it->ooi_dirn++;
		if (rc == 0) {
			it->ooi_leaf_open = true;
			return 0;
		}
		if (rc != -ENOENT)
			return rc;
	}
}

/*
 * Derive the FID of an O/ tree entry. The object's trusted.lma is
 * authoritative; the path is only a hint, so an object without one is skipped
 * rather than guessed at.
 */
static int osd_ot_fid_from_leaf(const struct lu_env *env,
				struct osd_otable_it *it,
				struct dentry *parent,
				const char *name, struct lu_fid *fid)
{
	struct qstr qstr = QSTR_INIT(name, strlen(name));
	struct dentry *dchild;
	int rc;

	inode_lock_shared(d_inode(parent));
	dchild = lookup_noperm(&qstr, parent);
	inode_unlock_shared(d_inode(parent));
	if (IS_ERR(dchild))
		return PTR_ERR(dchild);
	if (d_really_is_negative(dchild)) {
		dput(dchild);
		return -ENOENT;
	}

	rc = osd_lma_get(env, d_inode(dchild), fid);
	dput(dchild);

	return rc;
}

/* advance to the next object; 0 = have one, 1 = end, <0 = error */
static int osd_ot_advance(const struct lu_env *env, struct osd_otable_it *it)
{
	struct osd_dir_ent *ent;
	int rc;

	it->ooi_valid = false;

	while (it->ooi_phase < 2) {
		if (it->ooi_phase == 0) {
			if (!it->ooi_leaf_open) {
				rc = osd_ot_next_leaf(it);
				if (rc < 0)
					return rc;
				if (rc == 1) {
					/* O/ tree exhausted, move to oi/ */
					it->ooi_phase = 1;
					rc = osd_dir_reader_init(&it->ooi_oi,
							it->ooi_dev,
							it->ooi_dev->od_oi->oi_root);
					if (rc == 0)
						it->ooi_oi_open = true;
					else if (rc != -ENOENT)
						return rc;
					continue;
				}
			}

			rc = osd_dir_reader_next(&it->ooi_leaf, &ent);
			if (rc < 0)
				return rc;
			if (rc == 1) {
				osd_ot_close_leaf(it);
				continue;
			}

			rc = osd_ot_fid_from_leaf(env, it,
						  it->ooi_seq->oos_dirs[it->ooi_dirn - 1],
						  ent->de_name, &it->ooi_fid);
			if (rc != 0)
				continue;	/* not ours; skip */
		} else {
			u64 seq;
			u32 oid, ver;

			if (!it->ooi_oi_open) {
				it->ooi_phase = 2;
				break;
			}

			rc = osd_dir_reader_next(&it->ooi_oi, &ent);
			if (rc < 0)
				return rc;
			if (rc == 1) {
				osd_ot_close_oi(it);
				it->ooi_phase = 2;
				break;
			}
			/* the staging directory is not an object */
			if (strcmp(ent->de_name, "stage") == 0)
				continue;
			if (sscanf(ent->de_name, "%llx:%x:%x", &seq, &oid,
				   &ver) != 3)
				continue;
			it->ooi_fid.f_seq = seq;
			it->ooi_fid.f_oid = oid;
			it->ooi_fid.f_ver = ver;
		}

		if (!fid_is_sane(&it->ooi_fid))
			continue;

		it->ooi_ordinal++;
		if (it->ooi_skip > 0) {
			it->ooi_skip--;
			continue;
		}

		it->ooi_valid = true;
		return 0;
	}

	return 1;
}

/* ------------------------------------------------------------------ */
/* dt_index_operations                                                 */
/* ------------------------------------------------------------------ */

static struct dt_it *osd_otable_it_init(const struct lu_env *env,
					struct dt_object *dt, __u32 attr)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_device *osd = osd_obj2dev(obj);
	struct osd_otable_it *it;

	ENTRY;

	OBD_ALLOC_PTR(it);
	if (it == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	it->ooi_dev = osd;
	it->ooi_obj = obj;
	it->ooi_phase = 0;
	lu_object_get(&dt->do_lu);

	RETURN((struct dt_it *)it);
}

static void osd_otable_it_fini(const struct lu_env *env, struct dt_it *di)
{
	struct osd_otable_it *it = (struct osd_otable_it *)di;

	osd_ot_close_leaf(it);
	osd_ot_close_oi(it);
	if (it->ooi_obj != NULL)
		lu_object_put(env, &it->ooi_obj->oo_dt.do_lu);
	OBD_FREE_PTR(it);
}

static int osd_otable_it_get(const struct lu_env *env, struct dt_it *di,
			     const struct dt_key *key)
{
	struct osd_otable_it *it = (struct osd_otable_it *)di;

	/* LFSCK signals it is ready to consume; ldiskfs does the same */
	it->ooi_ready = true;
	return 0;
}

static void osd_otable_it_put(const struct lu_env *env, struct dt_it *di)
{
}

static int osd_otable_it_next(const struct lu_env *env, struct dt_it *di)
{
	struct osd_otable_it *it = (struct osd_otable_it *)di;

	return osd_ot_advance(env, it);
}

static struct dt_key *osd_otable_it_key(const struct lu_env *env,
					const struct dt_it *di)
{
	return NULL;
}

static int osd_otable_it_key_size(const struct lu_env *env,
				  const struct dt_it *di)
{
	return sizeof(u64);
}

static int osd_otable_it_rec(const struct lu_env *env, const struct dt_it *di,
			     struct dt_rec *rec, __u32 attr)
{
	struct osd_otable_it *it = (struct osd_otable_it *)di;

	if (!it->ooi_valid)
		return -ENOENT;

	*(struct lu_fid *)rec = it->ooi_fid;
	return 0;
}

static __u64 osd_otable_it_store(const struct lu_env *env,
				 const struct dt_it *di)
{
	struct osd_otable_it *it = (struct osd_otable_it *)di;

	return it->ooi_ordinal;
}

static int osd_otable_it_load(const struct lu_env *env,
			      const struct dt_it *di, __u64 hash)
{
	struct osd_otable_it *it = (struct osd_otable_it *)di;
	int rc;

	/* restart the walk and skip forward to the checkpoint */
	osd_ot_close_leaf(it);
	osd_ot_close_oi(it);
	it->ooi_phase = 0;
	it->ooi_seq = NULL;
	it->ooi_dirn = 0;
	it->ooi_ordinal = 0;
	it->ooi_skip = hash;
	it->ooi_valid = false;
	it->ooi_ready = true;

	/*
	 * Same convention as the directory iterator: >0 means positioned on a
	 * record, 0 means the caller should call ->next().
	 */
	rc = osd_ot_advance(env, it);
	if (rc == 0)
		return 1;
	if (rc == 1)
		return 0;
	return rc;
}

const struct dt_index_operations osd_otable_ops = {
	.dio_it = {
		.init		= osd_otable_it_init,
		.fini		= osd_otable_it_fini,
		.get		= osd_otable_it_get,
		.put		= osd_otable_it_put,
		.next		= osd_otable_it_next,
		.key		= osd_otable_it_key,
		.key_size	= osd_otable_it_key_size,
		.rec		= osd_otable_it_rec,
		.store		= osd_otable_it_store,
		.load		= osd_otable_it_load,
	}
};
