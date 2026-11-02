// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Quota for osd-xfs: accounting, project IDs, and the hooks the quota slave
 * device needs.
 *
 * The accounting index is a *virtual* index. Lustre asks the OSD "how much
 * space and how many inodes does this uid/gid/projid own", and the OSD is
 * expected to answer from the backing filesystem's own quota records rather
 * than from anything Lustre stores. osd-ldiskfs answers by walking the ext4
 * quota file tree directly, which costs it 334 lines of on-disk format
 * knowledge in osd_quota_fmt.c.
 *
 * None of that is necessary. The VFS already has the two primitives:
 *
 *   sb->s_qcop->get_dqblk(sb, qid, &dqblk)         one id
 *   sb->s_qcop->get_nextdqblk(sb, &qid, &dqblk)    the next id that exists
 *
 * Both are reached through the superblock, so no filesystem has to export
 * anything, and XFS implements all of them (fs/xfs/xfs_quotaops.c). The second
 * one is what makes iteration possible without knowing the on-disk layout --
 * ldiskfs predates it.
 *
 * XFS quota has to be turned on at mount time, so osd_mount() passes
 * uqnoenforce,gqnoenforce,pqnoenforce: accounting only, since Lustre enforces
 * through the quota slave. Unlike ext4 there is no on-disk quota file to
 * create at mkfs time; the accounting lives in the filesystem's own metadata.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/fs.h>
#include <linux/quota.h>
#include <linux/quotaops.h>
#include <linux/fileattr.h>

#include <lustre_quota.h>

#include "osd_internal.h"

/*
 * Which quota type an accounting object refers to. osd-ldiskfs has a private
 * fid2type() for this; the OIDs are in lustre_fid.h.
 */
static int osd_quota_type(const struct lu_fid *fid)
{
	switch (fid_oid(fid)) {
	case ACCT_USER_OID:
		return USRQUOTA;
	case ACCT_GROUP_OID:
		return GRPQUOTA;
	case ACCT_PROJECT_OID:
		return PRJQUOTA;
	default:
		return -1;
	}
}

static bool osd_quota_enabled(struct super_block *sb, int type)
{
	struct qc_state state;

	if (sb->s_qcop == NULL || sb->s_qcop->get_state == NULL)
		return false;
	if (sb->s_qcop->get_state(sb, &state) != 0)
		return false;

	return !!(state.s_state[type].flags & QCI_ACCT_ENABLED);
}

/* ------------------------------------------------------------------ */
/* the accounting index                                                */
/* ------------------------------------------------------------------ */

static int osd_acct_index_lookup(const struct lu_env *env,
				 struct dt_object *dt, struct dt_rec *rec,
				 const struct dt_key *key)
{
	struct super_block *sb = osd_sb(osd_obj2dev(osd_dt_obj(dt)));
	struct lquota_acct_rec *acct = (struct lquota_acct_rec *)rec;
	__u64 id = *((__u64 *)key);
	struct qc_dqblk dqblk;
	struct kqid qid;
	int type;
	int rc;

	ENTRY;

	type = osd_quota_type(lu_object_fid(&dt->do_lu));
	if (type < 0)
		RETURN(-EINVAL);
	if (sb->s_qcop == NULL || sb->s_qcop->get_dqblk == NULL)
		RETURN(-EOPNOTSUPP);

	memset(&dqblk, 0, sizeof(dqblk));
	qid = make_kqid(&init_user_ns, type, id);
	if (!qid_has_mapping(&init_user_ns, qid))
		RETURN(-EINVAL);

	rc = sb->s_qcop->get_dqblk(sb, qid, &dqblk);
	if (rc == -ENOENT || rc == -ESRCH) {
		/*
		 * No dquot for this id, which means it owns nothing.
		 *
		 * XFS reports that as -ENOENT (xfs_qm_scall_getquota() calls
		 * xfs_qm_dqget() without doalloc). ext4 instead hands back a
		 * zeroed dqblk, so osd-ldiskfs never has to think about it.
		 * Propagating the error turns "this user owns nothing" into a
		 * failed quotactl, and the client then prints every figure in
		 * brackets as inaccurate -- including the ids that do have
		 * records, because one OST erroring poisons the whole report.
		 */
		acct->bspace = 0;
		acct->ispace = 0;
		RETURN(+1);
	}
	if (rc)
		RETURN(rc);

	acct->bspace = dqblk.d_space + dqblk.d_rt_space;
	acct->ispace = dqblk.d_ino_count;

	/* dt_lookup() maps >0 to "found" */
	RETURN(+1);
}

/*
 * Iteration over every id that has accounting. get_nextdqblk() advances the
 * qid to the first one at or after it that exists, which is exactly the
 * semantics ->next() wants and removes any need to understand the on-disk
 * quota format.
 */
struct osd_it_quota {
	struct osd_object	*oiq_obj;
	int			 oiq_type;
	__u64			 oiq_id;
	struct qc_dqblk		 oiq_dqblk;
	bool			 oiq_valid;
};

static struct dt_it *osd_it_acct_init(const struct lu_env *env,
				      struct dt_object *dt, __u32 attr)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_it_quota *it;
	int type;

	ENTRY;

	type = osd_quota_type(lu_object_fid(&dt->do_lu));
	if (type < 0)
		RETURN(ERR_PTR(-EINVAL));

	OBD_ALLOC_PTR(it);
	if (it == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	it->oiq_obj = obj;
	it->oiq_type = type;
	it->oiq_id = 0;
	lu_object_get(&dt->do_lu);

	RETURN((struct dt_it *)it);
}

static void osd_it_acct_fini(const struct lu_env *env, struct dt_it *di)
{
	struct osd_it_quota *it = (struct osd_it_quota *)di;

	if (it->oiq_obj != NULL)
		lu_object_put(env, &it->oiq_obj->oo_dt.do_lu);
	OBD_FREE_PTR(it);
}

/* find the first id at or after @from that has accounting */
static int osd_it_acct_seek(struct osd_it_quota *it, __u64 from)
{
	struct super_block *sb = osd_sb(osd_obj2dev(it->oiq_obj));
	struct kqid qid;
	int rc;

	it->oiq_valid = false;

	if (sb->s_qcop == NULL || sb->s_qcop->get_nextdqblk == NULL)
		return -EOPNOTSUPP;

	qid = make_kqid(&init_user_ns, it->oiq_type, from);
	if (!qid_has_mapping(&init_user_ns, qid))
		return -EINVAL;

	memset(&it->oiq_dqblk, 0, sizeof(it->oiq_dqblk));
	rc = sb->s_qcop->get_nextdqblk(sb, &qid, &it->oiq_dqblk);
	if (rc == -ENOENT || rc == -ESRCH)
		return 1;		/* no more ids */
	if (rc)
		return rc;

	it->oiq_id = from_kqid(&init_user_ns, qid);
	it->oiq_valid = true;

	return 0;
}

static int osd_it_acct_get(const struct lu_env *env, struct dt_it *di,
			   const struct dt_key *key)
{
	struct osd_it_quota *it = (struct osd_it_quota *)di;

	it->oiq_id = key ? *((__u64 *)key) : 0;
	it->oiq_valid = false;

	return 0;
}

static void osd_it_acct_put(const struct lu_env *env, struct dt_it *di)
{
}

static int osd_it_acct_next(const struct lu_env *env, struct dt_it *di)
{
	struct osd_it_quota *it = (struct osd_it_quota *)di;
	__u64 from = it->oiq_valid ? it->oiq_id + 1 : it->oiq_id;

	/*
	 * Guard against wrapping at the top of the id space, which would
	 * restart the walk from 0 and never terminate.
	 */
	if (it->oiq_valid && it->oiq_id == U32_MAX)
		return 1;

	return osd_it_acct_seek(it, from);
}

static struct dt_key *osd_it_acct_key(const struct lu_env *env,
				      const struct dt_it *di)
{
	struct osd_it_quota *it = (struct osd_it_quota *)di;

	return (struct dt_key *)&it->oiq_id;
}

static int osd_it_acct_key_size(const struct lu_env *env,
				const struct dt_it *di)
{
	return sizeof(__u64);
}

static int osd_it_acct_rec(const struct lu_env *env, const struct dt_it *di,
			   struct dt_rec *rec, __u32 attr)
{
	struct osd_it_quota *it = (struct osd_it_quota *)di;
	struct lquota_acct_rec *acct = (struct lquota_acct_rec *)rec;

	if (!it->oiq_valid)
		return -ENOENT;

	acct->bspace = it->oiq_dqblk.d_space + it->oiq_dqblk.d_rt_space;
	acct->ispace = it->oiq_dqblk.d_ino_count;

	return 0;
}

static __u64 osd_it_acct_store(const struct lu_env *env,
			       const struct dt_it *di)
{
	struct osd_it_quota *it = (struct osd_it_quota *)di;

	return it->oiq_id;
}

static int osd_it_acct_load(const struct lu_env *env, const struct dt_it *di,
			    __u64 hash)
{
	struct osd_it_quota *it = (struct osd_it_quota *)di;
	int rc;

	rc = osd_it_acct_seek(it, hash);
	if (rc == 0)
		return 1;		/* positioned on a record */
	if (rc == 1)
		return 0;		/* nothing there */
	return rc;
}

const struct dt_index_operations osd_acct_index_ops = {
	.dio_lookup	= osd_acct_index_lookup,
	.dio_it		= {
		.init		= osd_it_acct_init,
		.fini		= osd_it_acct_fini,
		.get		= osd_it_acct_get,
		.put		= osd_it_acct_put,
		.next		= osd_it_acct_next,
		.key		= osd_it_acct_key,
		.key_size	= osd_it_acct_key_size,
		.rec		= osd_it_acct_rec,
		.store		= osd_it_acct_store,
		.load		= osd_it_acct_load,
	}
};

/*
 * Whether an accounting index can be served at all. Returned to
 * osd_index_try() so a filesystem mounted without quota says so instead of
 * handing back an index that reports zero for everything -- a wrong answer
 * that looks like an answer.
 */
bool osd_acct_available(struct osd_device *osd, const struct lu_fid *fid)
{
	int type = osd_quota_type(fid);

	if (type < 0)
		return false;

	return osd_quota_enabled(osd_sb(osd), type);
}

/* ------------------------------------------------------------------ */
/* project id                                                          */
/* ------------------------------------------------------------------ */

/*
 * The project id is reached through i_op->fileattr_get/set, the same interface
 * chattr(1) uses, so this needs nothing from XFS either. ldiskfs added a
 * virtual trusted.projid xattr for the same purpose before that interface
 * existed.
 */
__u32 osd_get_projid(struct inode *inode)
{
	struct file_kattr fa = {};
	struct dentry *dentry;
	__u32 projid = 0;

	if (inode->i_op->fileattr_get == NULL)
		return 0;

	dentry = d_find_any_alias(inode);
	if (dentry == NULL)
		return 0;

	if (inode->i_op->fileattr_get(dentry, &fa) == 0 && fa.fsx_valid)
		projid = fa.fsx_projid;

	dput(dentry);

	return projid;
}

int osd_set_projid(struct inode *inode, __u32 projid)
{
	struct file_kattr fa = {};
	struct dentry *dentry;
	int rc;

	if (inode->i_op->fileattr_set == NULL ||
	    inode->i_op->fileattr_get == NULL)
		return -EOPNOTSUPP;

	dentry = d_find_any_alias(inode);
	if (dentry == NULL)
		return -ENOENT;

	/*
	 * Read first: fileattr_set replaces the whole fsxattr, so setting only
	 * the project id would otherwise clear the extent size hints and the
	 * xflags along with it.
	 */
	rc = inode->i_op->fileattr_get(dentry, &fa);
	if (rc == 0) {
		fa.fsx_projid = projid;
		fa.fsx_valid = true;
		fa.flags_valid = false;
		inode_lock(inode);
		rc = inode->i_op->fileattr_set(&nop_mnt_idmap, dentry, &fa);
		inode_unlock(inode);
	}

	dput(dentry);

	return rc;
}

/* ------------------------------------------------------------------ */
/* enforcement: the declare-phase hooks the quota slave needs          */
/* ------------------------------------------------------------------ */

/*
 * Ask the quota slave whether @qi's id may consume qi->lqi_space, and record
 * the reservation so trans_stop can release it.
 *
 * Without this, quota limits are recorded and reported but never applied: a
 * 32 MiB limit let a 300 MiB write through untouched, because nothing in the
 * write path ever consulted the slave. The accounting index answers "how much
 * is used"; this is what answers "may you use more".
 */
static int osd_declare_qid(const struct lu_env *env, struct osd_thandle *oh,
			   struct lquota_id_info *qi, struct osd_object *obj,
			   enum osd_quota_local_flags *local_flags)
{
	struct osd_device *dev = osd_dt_dev(oh->ot_super.th_dev);
	struct qsd_instance *qsd = dev->od_quota_slave_dt;
	int i, rc;

	ENTRY;

	/*
	 * qsd_op_begin() dereferences the lquota_trans without checking it, so
	 * a transaction that somehow reached here without one must not be
	 * passed on: it is a NULL dereference inside lquota, reported as a
	 * crash in qsd_op_begin() with no hint that the caller is at fault.
	 */
	if (qsd == NULL || oh->ot_quota_trans == NULL)
		RETURN(0);

	/*
	 * Declaring the same id twice in one transaction would reserve twice
	 * and then release once.
	 */
	for (i = 0; i < oh->ot_id_cnt; i++) {
		if (oh->ot_id_array[i].oq_type == qi->lqi_type &&
		    oh->ot_id_array[i].oq_blk == !!qi->lqi_is_blk &&
		    oh->ot_id_array[i].oq_id.qid_uid == qi->lqi_id.qid_uid)
			RETURN(0);
	}

	if (oh->ot_id_cnt >= OSD_MAX_UGID_CNT) {
		CERROR("%s: too many quota ids in one transaction\n",
		       osd_name(dev));
		RETURN(-EOVERFLOW);
	}

	rc = qsd_op_begin(env, qsd, oh->ot_quota_trans, qi, local_flags);
	CDEBUG(D_QUOTA, "declare type=%d id=%llu space=%lld: rc = %d\n",
	       qi->lqi_type, (unsigned long long)qi->lqi_id.qid_uid,
	       qi->lqi_space, rc);
	if (rc == 0 || rc == -EDQUOT || rc == -EINPROGRESS) {
		oh->ot_id_array[oh->ot_id_cnt].oq_id = qi->lqi_id;
		oh->ot_id_array[oh->ot_id_cnt].oq_type = qi->lqi_type;
		oh->ot_id_array[oh->ot_id_cnt].oq_blk = !!qi->lqi_is_blk;
		oh->ot_id_cnt++;
	}

	RETURN(rc);
}

int osd_declare_inode_qid(const struct lu_env *env, qid_t uid, qid_t gid,
			  __u32 projid, long long space,
			  struct osd_thandle *oh, struct osd_object *obj,
			  enum osd_quota_local_flags *local_flags, bool blk)
{
	struct osd_thread_info *info = osd_oti_get(env);
	struct lquota_id_info *qi = &info->oti_qi;
	bool force = oh->ot_super.th_ignore_quota;
	int rcu, rcg, rcp;

	ENTRY;

	/*
	 * Nothing owned by root is subject to quota, and the internal objects
	 * (llog, last_rcvd, the OI) are all root-owned, so this skips the
	 * common case entirely.
	 */
	if (uid == 0 && gid == 0 && projid == 0)
		RETURN(0);

	memset(qi, 0, sizeof(*qi));
	qi->lqi_space = space;
	qi->lqi_is_blk = blk;

	qi->lqi_id.qid_uid = uid;
	qi->lqi_type = USRQUOTA;
	rcu = osd_declare_qid(env, oh, qi, obj, local_flags);
	if (force && (rcu == -EDQUOT || rcu == -EINPROGRESS))
		rcu = 0;
	/*
	 * Carry on to the group and project ids even after EDQUOT when the
	 * caller wants the flags, so it learns about every id that is over --
	 * osd_declare_write_commit() uses that to mark the buffers.
	 */
	if (rcu && (rcu != -EDQUOT || local_flags == NULL))
		RETURN(rcu);

	qi->lqi_id.qid_gid = gid;
	qi->lqi_type = GRPQUOTA;
	rcg = osd_declare_qid(env, oh, qi, obj, local_flags);
	if (force && (rcg == -EDQUOT || rcg == -EINPROGRESS))
		rcg = 0;
	if (rcg && (rcg != -EDQUOT || local_flags == NULL))
		RETURN(rcu ? rcu : rcg);

	qi->lqi_id.qid_projid = projid;
	qi->lqi_type = PRJQUOTA;
	rcp = osd_declare_qid(env, oh, qi, obj, local_flags);
	if (force && (rcp == -EDQUOT || rcp == -EINPROGRESS))
		rcp = 0;

	RETURN(rcu ? rcu : (rcg ? rcg : rcp));
}
