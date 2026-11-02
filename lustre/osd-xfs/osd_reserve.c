// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Advisory filesystem-space admission for the dt declare phase.
 *
 * Declarations accumulate estimated filesystem blocks and inodes. At
 * trans_start, a fresh statfs sample is compared with those estimates and
 * outstanding OSD claims. A failed sample refuses admission; it must not
 * silently disable the check.
 *
 * This does NOT reserve XFS allocation-group space, log tickets or metadata
 * btree growth. Even with no external writers, XFS can
 * fail an admitted operation because its native reservations and allocation
 * constraints differ from these scalar estimates. It is an early ENOSPC
 * check, not a guarantee of execution success.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/blkdev.h>

#include "osd_internal.h"

static const char *const osd_op_names[OSD_OT_MAX] = {
	[OSD_OT_ATTR_SET]	= "attr_set",
	[OSD_OT_XATTR_SET]	= "xattr_set",
	[OSD_OT_XATTR_DEL]	= "xattr_del",
	[OSD_OT_CREATE]		= "create",
	[OSD_OT_DESTROY]	= "destroy",
	[OSD_OT_REF_ADD]	= "ref_add",
	[OSD_OT_REF_DEL]	= "ref_del",
	[OSD_OT_INSERT]		= "insert",
	[OSD_OT_DELETE]		= "delete",
	[OSD_OT_WRITE]		= "write",
	[OSD_OT_PUNCH]		= "punch",
};

const char *osd_op_name(enum osd_op_type op)
{
	if (op < 0 || op >= OSD_OT_MAX)
		return "?";

	return osd_op_names[op] ?: "?";
}

/*
 * Add an operation's cost to what this transaction will ask for.
 *
 * Called from the dt_declare_* methods, which run before trans_start, so this
 * only ever touches the caller's own thandle and needs no locking.
 */
void osd_declare_op(struct thandle *th, enum osd_op_type op, __u64 blocks,
		    __u32 inodes)
{
	struct osd_thandle *oh;

	if (th == NULL)
		return;

	oh = osd_thandle(th);

	/*
	 * Declaring after trans_start is a bug in the caller, not something to
	 * accommodate: the reservation has already been taken, so the extra
	 * cost is not covered by anything. Say so rather than silently
	 * accepting it.
	 */
	if (unlikely(oh->ot_started || oh->ot_res_claimed)) {
		CWARN("%s: %s declared after the transaction started\n",
		      osd_name(osd_dt_dev(th->th_dev)), osd_op_name(op));
		oh->ot_res_error = -EINVAL;
		return;
	}
	if (unlikely(op < 0 || op >= OSD_OT_MAX)) {
		oh->ot_res_error = -EINVAL;
		return;
	}
	if (unlikely(blocks > S64_MAX - oh->ot_res.or_blocks ||
		     inodes > U32_MAX - oh->ot_res.or_inodes ||
		     oh->ot_declare_ops[op] == INT_MAX)) {
		oh->ot_res_error = -EOVERFLOW;
		return;
	}

	oh->ot_res.or_blocks += blocks;
	oh->ot_res.or_inodes += inodes;
	oh->ot_declare_ops[op]++;
}

/*
 * Record that an operation actually ran.
 *
 * Only used to catch a declare phase that has drifted out of step with the
 * execute phase, which is the failure mode that matters here: an operation that
 * executes without having been declared is consuming space nobody reserved.
 * osd-ldiskfs has the same check on its credits, and it exists there because
 * under-declaring is a recurring bug rather than a theoretical one.
 */
void osd_exec_op(const struct lu_env *env, struct thandle *th,
		 enum osd_op_type op, __u64 blocks, __u32 inodes)
{
	struct osd_device *osd;
	struct osd_thandle *oh;

	if (th == NULL || op < 0 || op >= OSD_OT_MAX)
		return;

	oh = osd_thandle(th);
	osd = osd_dt_dev(th->th_dev);

	oh->ot_exec_ops[op]++;
	oh->ot_used.or_blocks += blocks;
	oh->ot_used.or_inodes += inodes;

	if (oh->ot_exec_ops[op] > oh->ot_declare_ops[op]) {
		atomic64_inc(&osd->od_res_undeclared);
		if (osd->od_declare_check)
			CWARN("%s: %s ran %d time(s) but was declared %d\n",
			      osd_name(osd), osd_op_name(op),
			      oh->ot_exec_ops[op], oh->ot_declare_ops[op]);
	}

	if (oh->ot_used.or_blocks > oh->ot_res.or_blocks ||
	    oh->ot_used.or_inodes > oh->ot_res.or_inodes) {
		atomic64_inc(&osd->od_res_overrun);
		if (osd->od_declare_check)
			CWARN("%s: used %llu blocks/%u inodes, declared %llu blocks/%u inodes\n",
			      osd_name(osd), oh->ot_used.or_blocks,
			      oh->ot_used.or_inodes, oh->ot_res.or_blocks,
			      oh->ot_res.or_inodes);
	}
}

/*
 * Do not use the display cache for admission: a completed transaction drops
 * its claim but its allocation remains. Reusing a stale free-space sample
 * would admit that same space again until the cache timer expires.
 */

static int osd_statfs_sample(struct osd_device *osd, struct kstatfs *ks)
{
	struct super_block *sb = osd_sb(osd);
	int rc;

	rc = sb->s_op->statfs(sb->s_root, ks);
	atomic64_inc(&osd->od_statfs_calls);
	return rc;
}

/*
 * Check estimated space before execution. This does not acquire a native
 * filesystem reservation; subsequent VFS calls can still fail.
 */
int osd_reserve_claim(const struct lu_env *env, struct osd_device *osd,
		      struct osd_thandle *oh)
{
	struct super_block *sb = osd_sb(osd);
	struct kstatfs ks = {};
	u64 free_blocks, free_inodes, promised, promised_inodes;
	int rc;

	ENTRY;
	if (oh->ot_res_error)
		RETURN(oh->ot_res_error);

	if (oh->ot_res_claimed)
		RETURN(0);

	/*
	 * Every early exit below leaves ot_res_claimed clear, and that is
	 * deliberate: osd_reserve_release() subtracts ot_res from the
	 * accumulator whenever the flag is set, so setting it on a path that
	 * never charged the accumulator drives it negative. Doing exactly that
	 * on the reserve_check path took od_res_blocks to -2636274.
	 */
	if (oh->ot_res.or_blocks == 0 && oh->ot_res.or_inodes == 0)
		RETURN(0);

	if (!osd->od_reserve_check)
		RETURN(0);

	if (sb == NULL || sb->s_op == NULL || sb->s_op->statfs == NULL)
		RETURN(-EOPNOTSUPP);

	/* Keep completed allocations charged until this sample is consumed. */
	mutex_lock(&osd->od_res_lock);
	rc = osd_statfs_sample(osd, &ks);
	if (rc != 0) {
		mutex_unlock(&osd->od_res_lock);
		RETURN(rc);
	}

	/* XFS currently reports f_bavail == f_bfree, after its own reserves. */
	free_blocks = ks.f_bavail;
	free_inodes = ks.f_ffree;

	/* Charge both resources together, without overflowing either counter. */
	promised = atomic64_read(&osd->od_res_blocks);
	promised_inodes = atomic64_read(&osd->od_res_inodes);
	if (promised > free_blocks ||
	    oh->ot_res.or_blocks > free_blocks - promised ||
	    promised_inodes > free_inodes ||
	    oh->ot_res.or_inodes > free_inodes - promised_inodes ||
	    oh->ot_res.or_blocks > S64_MAX - promised ||
	    oh->ot_res.or_inodes > S64_MAX - promised_inodes) {
		mutex_unlock(&osd->od_res_lock);
		atomic64_inc(&osd->od_res_denied);
		CDEBUG(D_INODE,
		       "%s: refusing %llu blocks/%u inodes: %llu blocks/%llu inodes free, %llu blocks/%llu inodes promised\n",
		       osd_name(osd), oh->ot_res.or_blocks,
		       oh->ot_res.or_inodes, free_blocks, free_inodes,
		       promised, promised_inodes);
		RETURN(-ENOSPC);
	}
	atomic64_add(oh->ot_res.or_blocks, &osd->od_res_blocks);
	atomic64_add(oh->ot_res.or_inodes, &osd->od_res_inodes);

	oh->ot_res_claimed = 1;
	mutex_unlock(&osd->od_res_lock);

	RETURN(0);
}

/*
 * Give the reservation back. Must be called for every transaction that claimed
 * one, whether it succeeded or not, or the accumulator leaks and the target
 * eventually refuses everything.
 */
void osd_reserve_release(struct osd_device *osd, struct osd_thandle *oh)
{
	if (!oh->ot_res_claimed)
		return;

	oh->ot_res_claimed = 0;
	mutex_lock(&osd->od_res_lock);

	if (oh->ot_res.or_blocks != 0)
		atomic64_sub(oh->ot_res.or_blocks, &osd->od_res_blocks);
	if (oh->ot_res.or_inodes != 0)
		atomic64_sub(oh->ot_res.or_inodes, &osd->od_res_inodes);
	mutex_unlock(&osd->od_res_lock);
}

void osd_reserve_init(struct osd_device *osd)
{
	mutex_init(&osd->od_res_lock);
	atomic64_set(&osd->od_res_blocks, 0);
	atomic64_set(&osd->od_res_inodes, 0);
	atomic64_set(&osd->od_res_denied, 0);
	atomic64_set(&osd->od_res_undeclared, 0);
	atomic64_set(&osd->od_res_overrun, 0);
	atomic64_set(&osd->od_dirty_flushed, 0);
	atomic64_set(&osd->od_dirty_txn, 0);
	atomic64_set(&osd->od_dirty_overflow, 0);
	osd->od_flush_buffered = 1;
	osd->od_reserve_check = 1;
	atomic64_set(&osd->od_statfs_calls, 0);
	/*
	 * On by default. The declares are new and the check is how a drift
	 * between them and the execute phase gets noticed at all; it costs a
	 * couple of comparisons per operation.
	 */
	osd->od_declare_check = 1;
}

/* ------------------------------------------------------------------ */
/* buffered writes that trans_stop has to flush                        */
/* ------------------------------------------------------------------ */

/*
 * Note an inode this transaction wrote with buffered I/O.
 *
 * Duplicates are dropped: a transaction typically writes last_rcvd several
 * times and there is no point flushing it more than once.
 */
void osd_trans_note_dirty(struct thandle *th, struct inode *inode)
{
	struct osd_thandle *oh;
	int i;

	if (th == NULL || inode == NULL)
		return;

	oh = osd_thandle(th);

	for (i = 0; i < oh->ot_ndirty; i++) {
		if (oh->ot_dirty[i] == inode)
			return;
	}

	if (oh->ot_ndirty >= OSD_TH_MAX_DIRTY) {
		/*
		 * More buffered writes than the array holds. Rather than
		 * allocate, remember that the list is incomplete and flush
		 * everything at trans_stop. Correct, slower, and rare.
		 */
		oh->ot_dirty_overflow = 1;
		return;
	}

	oh->ot_dirty[oh->ot_ndirty++] = igrab(inode);
}

/*
 * Drop the inode references this transaction is holding.
 *
 * Must run for every transaction, on every path, including the failures.
 * Leaking an igrab() keeps the inode alive, which keeps its dquot alive, and
 * unmount then spins for ever in xfs_qm_dquot_walk() waiting to purge it. That
 * is exactly what happened when this was first written with the flush inside
 * an "if (rc == 0)" branch.
 */
void osd_trans_drop_dirty(struct osd_thandle *oh)
{
	int i;

	for (i = 0; i < oh->ot_ndirty; i++) {
		if (oh->ot_dirty[i] != NULL)
			iput(oh->ot_dirty[i]);
		oh->ot_dirty[i] = NULL;
	}
	oh->ot_ndirty = 0;
	oh->ot_dirty_overflow = 0;
}

/*
 * Write this transaction's buffered pages out to the device.
 *
 * Deliberately does NOT issue a device cache flush: the caller does that once,
 * because a cache flush is the expensive part and a batch of transactions
 * needs only one of them. Returns true if anything was written and a flush is
 * therefore owed.
 */
bool osd_trans_writeback_dirty(const struct lu_env *env,
			       struct osd_device *osd,
			       struct osd_thandle *oh, int *rcp)
{
	bool written = false;
	int rc = 0;
	int i;

	/*
	 * The tunable exists to attribute cost, not to tune. With it off, a
	 * commit callback can report a transno durable while it is still only
	 * in the page cache.
	 */
	if (!osd->od_flush_buffered) {
		osd_trans_drop_dirty(oh);
		return false;
	}

	if (oh->ot_ndirty > 0 || oh->ot_dirty_overflow)
		atomic64_inc(&osd->od_dirty_txn);

	if (unlikely(oh->ot_dirty_overflow)) {
		struct super_block *sb = osd_sb(osd);

		/*
		 * More buffered writes in one transaction than the array
		 * holds, so the tracked list is incomplete and flushing only
		 * what is in it would miss some. Flush everything instead:
		 * correct, much slower, and it should never happen. The
		 * counter is there to prove that.
		 */
		atomic64_inc(&osd->od_dirty_overflow);
		CWARN("%s: more than %d buffered writes in one transaction, flushing the whole filesystem\n",
		      osd_name(osd), OSD_TH_MAX_DIRTY);
		if (sb != NULL)
			rc = sync_filesystem(sb);
		written = true;
	}

	for (i = 0; i < oh->ot_ndirty; i++) {
		struct inode *inode = oh->ot_dirty[i];
		int rc2;

		if (inode == NULL)
			continue;
		rc2 = filemap_write_and_wait(inode->i_mapping);
		atomic64_inc(&osd->od_dirty_flushed);
		written = true;
		if (rc2 != 0 && rc == 0)
			rc = rc2;
		iput(inode);
		oh->ot_dirty[i] = NULL;
	}
	oh->ot_ndirty = 0;
	oh->ot_dirty_overflow = 0;

	if (rcp != NULL && rc != 0 && *rcp == 0)
		*rcp = rc;

	return written;
}

/*
 * Write back the inodes tracked by a whole batch of transactions, flushing
 * each distinct inode once.
 *
 * Every write transaction dirties last_rcvd, so flushing per transaction
 * issues one 4 KiB write to the same block per RPC. On a rotating device
 * that is a seek away from the sequential data stream and back, measured at
 * a fifth of all requests reaching the drive. Deduplicating across the batch
 * turns those N writes into one.
 *
 * Durability is unchanged: every inode any transaction in the batch dirtied
 * is written and waited for before this returns, and the caller runs it
 * before the commit callbacks that report those transactions durable.
 */
bool osd_commit_writeback_batch(struct osd_device *osd,
				struct list_head *batch, int *rcp)
{
	struct osd_thandle *oh;
	bool written = false;
	bool overflow = false;
	int rc = 0;

	if (!osd->od_flush_buffered) {
		list_for_each_entry(oh, batch, ot_commit_link)
			osd_trans_drop_dirty(oh);
		return false;
	}

	list_for_each_entry(oh, batch, ot_commit_link) {
		if (oh->ot_ndirty > 0 || oh->ot_dirty_overflow)
			atomic64_inc(&osd->od_dirty_txn);
		if (unlikely(oh->ot_dirty_overflow))
			overflow = true;
	}

	if (unlikely(overflow)) {
		struct super_block *sb = osd_sb(osd);

		/*
		 * A transaction tracked more buffered writes than its array
		 * holds, so its list is incomplete and flushing only what is
		 * in it would miss some. One filesystem-wide sync covers
		 * every inode in the batch, so nothing else is needed.
		 */
		atomic64_inc(&osd->od_dirty_overflow);
		CWARN("%s: more than %d buffered writes in one transaction, flushing the whole filesystem\n",
		      osd_name(osd), OSD_TH_MAX_DIRTY);
		if (sb != NULL)
			rc = sync_filesystem(sb);
		list_for_each_entry(oh, batch, ot_commit_link)
			osd_trans_drop_dirty(oh);
		if (rcp != NULL && rc != 0 && *rcp == 0)
			*rcp = rc;
		return true;
	}

	list_for_each_entry(oh, batch, ot_commit_link) {
		int i;

		for (i = 0; i < oh->ot_ndirty; i++) {
			struct inode *inode = oh->ot_dirty[i];
			struct osd_thandle *prev;
			bool seen = false;
			int rc2;

			if (inode == NULL)
				continue;

			/*
			 * Flushed already by an earlier transaction in this
			 * batch? The batch is a handful of transactions and
			 * each tracks one or two inodes, so the scan is
			 * cheaper than the allocation a set would need.
			 */
			list_for_each_entry(prev, batch, ot_commit_link) {
				int j;

				if (prev == oh)
					break;
				for (j = 0; j < prev->ot_ndirty; j++) {
					if (prev->ot_dirty[j] == inode) {
						seen = true;
						break;
					}
				}
				if (seen)
					break;
			}
			if (seen)
				continue;

			rc2 = filemap_write_and_wait(inode->i_mapping);
			atomic64_inc(&osd->od_dirty_flushed);
			written = true;
			if (rc2 != 0 && rc == 0)
				rc = rc2;
		}
	}

	/*
	 * Drop the references only after every flush, because the scan above
	 * compares against inodes still held by earlier transactions.
	 */
	list_for_each_entry(oh, batch, ot_commit_link)
		osd_trans_drop_dirty(oh);

	if (rcp != NULL && rc != 0 && *rcp == 0)
		*rcp = rc;

	return written;
}
