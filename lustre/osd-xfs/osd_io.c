// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * struct dt_body_operations for osd-xfs: record I/O and the bulk (BRW) path.
 *
 * Bulk reads and writes use filesystem-managed iterators over anonymous
 * request pages; XFS owns the extent mapping, page cache and I/O completion.
 * Writes request data-sync completion, including buffered fallback, because
 * O_DIRECT alone does not make data durable. Data is therefore on the device
 * before the transaction that records it (last_rcvd) is logged.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/uio.h>
#include <linux/bvec.h>

#include <lustre_compat.h>

#include "osd_internal.h"

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static struct file *osd_object_open(struct osd_object *obj, int flags)
{
	struct osd_device *osd = osd_obj2dev(obj);

	if (obj->oo_dentry == NULL || obj->oo_inode == NULL)
		return ERR_PTR(-ENOENT);

	return dentry_open(&(struct path){ .mnt = osd->od_mnt,
					   .dentry = obj->oo_dentry },
			   flags | O_LARGEFILE, current_cred());
}

static ssize_t osd_read(const struct lu_env *env, struct dt_object *dt,
			struct lu_buf *buf, loff_t *pos)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct file *file;
	ssize_t rc;

	file = osd_object_open(obj, O_RDONLY);
	if (IS_ERR(file))
		return PTR_ERR(file);

	rc = kernel_read(file, buf->lb_buf, buf->lb_len, pos);
	fput(file);

	return rc;
}

static ssize_t osd_declare_write(const struct lu_env *env,
				 struct dt_object *dt,
				 const struct lu_buf *buf, loff_t pos,
				 struct thandle *handle)
{
	struct osd_object *obj = osd_dt_obj(dt);
	u64 blocksize = osd_sb(osd_obj2dev(obj))->s_blocksize;
	u64 blocks = DIV_ROUND_UP_ULL(buf->lb_len, blocksize) + 1;

	/* pos may be -1 and lb_buf NULL during reply-data declaration. */
	osd_declare_op(handle, OSD_OT_WRITE, blocks, 0);
	return 0;
}

static ssize_t osd_write(const struct lu_env *env, struct dt_object *dt,
			 const struct lu_buf *buf, loff_t *pos,
			 struct thandle *handle)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct file *file;
	ssize_t rc;
	if (handle) {
		rc = osd_commit_error(osd_obj2dev(obj), 0);
		if (rc)
			return rc;
		osd_exec_op(env, handle, OSD_OT_WRITE, 0, 0);
	}

	/*
	 * Not O_DSYNC when a transaction is carrying the write. Every write
	 * RPC updates last_rcvd, so a per-write data-sync puts one 4 KiB
	 * write to the same block on the platter per RPC, which on a
	 * rotating device is a seek out of the sequential data stream and
	 * back. The transaction's commit path writes the file back instead,
	 * before the callbacks that report the transaction durable, so the
	 * durability boundary is unchanged.
	 *
	 * Without a transaction there is nothing to carry the sync, so the
	 * write has to be durable when it returns.
	 */
	file = osd_object_open(obj, O_WRONLY | (handle != NULL ? 0 : O_DSYNC));
	if (IS_ERR(file))
		return PTR_ERR(file);

	osd_trans_note_dirty(handle, obj->oo_inode);
	rc = kernel_write(file, buf->lb_buf, buf->lb_len, pos);
	fput(file);

	return rc;
}

/* ------------------------------------------------------------------ */
/* bulk I/O                                                            */
/* ------------------------------------------------------------------ */

/* Free the anonymous request pages osd_bufs_get() handed out. */
static void osd_bufs_free(struct niobuf_local *lnb, int nr)
{
	int i;

	for (i = 0; i < nr; i++) {
		if (lnb[i].lnb_page == NULL)
			continue;
		__free_page(lnb[i].lnb_page);
		lnb[i].lnb_page = NULL;
	}
}

static int osd_bufs_get(const struct lu_env *env, struct dt_object *dt,
			loff_t pos, ssize_t len, struct niobuf_local *lnb,
			int maxlnb, enum dt_bufs_type rw)
{
	struct osd_object *obj = osd_dt_obj(dt);
	int npages = 0;

	ENTRY;

	if (obj->oo_inode == NULL)
		RETURN(-ENOENT);
	if (unlikely(len <= 0))
		RETURN(-EINVAL);

	while (len > 0) {
		int poff = pos & (PAGE_SIZE - 1);
		int plen = min_t(int, PAGE_SIZE - poff, len);
		struct page *page;

		if (npages >= maxlnb) {
			osd_bufs_free(lnb, npages);
			RETURN(-EOVERFLOW);
		}

		page = alloc_page(GFP_NOFS);
		if (page == NULL) {
			osd_bufs_free(lnb, npages);
			RETURN(-ENOMEM);
		}

		lnb[npages].lnb_file_offset = pos;
		lnb[npages].lnb_page_offset = poff;
		lnb[npages].lnb_len = plen;
		lnb[npages].lnb_page = page;
		lnb[npages].lnb_data = NULL;
		lnb[npages].lnb_rc = 0;
		lnb[npages].lnb_flags = 0;
		lnb[npages].lnb_locked = 0;
		lnb[npages].lnb_hole = 0;
		lnb[npages].lnb_dio = 0;

		npages++;
		pos += plen;
		len -= plen;
	}

	RETURN(npages);
}

static int osd_bufs_put(const struct lu_env *env, struct dt_object *dt,
			struct niobuf_local *lnb, int npages)
{
	osd_bufs_free(lnb, npages);
	return 0;
}

/*
 * Bulk transfer.
 *
 * Lustre hands us an array of pages with file offsets. Rather than copying each
 * one with kernel_read/kernel_write, build a bio_vec iterator over a run of
 * contiguous pages and issue a single scatter/gather transfer. iov_iter_bvec()
 * and vfs_iter_read/write() are exported, so this needs no kernel change.
 *
 * Writes and reads go through the filesystem page cache. Writes use
 * RWF_DSYNC: generic_write_sync() invokes XFS fsync, which handles
 * data-device cache flushes even with an external log or when forcing the
 * log is a no-op.
 */

/* how many contiguous lnbs start at @i */
static int osd_bulk_run(struct niobuf_local *lnb, int npages, int i,
			size_t *bytes)
{
	loff_t next = lnb[i].lnb_file_offset;
	int n = 0;

	*bytes = 0;
	while (i + n < npages &&
	       lnb[i + n].lnb_file_offset == next &&
	       lnb[i + n].lnb_len > 0) {
		next += lnb[i + n].lnb_len;
		*bytes += lnb[i + n].lnb_len;
		n++;
	}
	return n;
}

static int osd_bulk_transfer(const struct lu_env *env, struct osd_object *obj,
			     struct niobuf_local *lnb, int npages, bool write)
{
	struct osd_device *osd = osd_obj2dev(obj);
	struct file *fbuf = NULL;
	struct bio_vec *bvec = NULL;
	loff_t isize;
	int i, rc = 0;

	ENTRY;

	if (obj->oo_inode == NULL || obj->oo_dentry == NULL)
		RETURN(-ENOENT);
	if (npages <= 0)
		RETURN(0);

	OBD_ALLOC_PTR_ARRAY_LARGE(bvec, npages);
	if (bvec == NULL)
		RETURN(-ENOMEM);

	fbuf = osd_object_open(obj, write ? O_WRONLY : O_RDONLY);
	if (IS_ERR(fbuf)) {
		rc = PTR_ERR(fbuf);
		fbuf = NULL;
		GOTO(out, rc);
	}

	isize = i_size_read(obj->oo_inode);

	for (i = 0; i < npages; ) {
		struct iov_iter iter;
		size_t bytes = 0;
		loff_t pos = lnb[i].lnb_file_offset;
		int n, j;
		ssize_t done;

		n = osd_bulk_run(lnb, npages, i, &bytes);
		if (n == 0) {		/* zero-length entry; skip it */
			i++;
			continue;
		}

		if (!write && pos >= isize) {
			/* wholly past EOF: hand back zeroes */
			for (j = 0; j < n; j++) {
				void *k = kmap_local_page(lnb[i + j].lnb_page);

				memset(k + lnb[i + j].lnb_page_offset, 0,
				       lnb[i + j].lnb_len);
				kunmap_local(k);
				lnb[i + j].lnb_rc = lnb[i + j].lnb_len;
				lnb[i + j].lnb_hole = 1;
			}
			i += n;
			continue;
		}

		if (!write && pos + bytes > isize)
			bytes = isize - pos;

		for (j = 0; j < n; j++)
			bvec_set_page(&bvec[j], lnb[i + j].lnb_page,
				      lnb[i + j].lnb_len,
				      lnb[i + j].lnb_page_offset);

		iov_iter_bvec(&iter, write ? ITER_SOURCE : ITER_DEST,
			      bvec, n, bytes);

		if (write)
			done = vfs_iter_write(fbuf, &iter, &pos, RWF_DSYNC);
		else
			done = vfs_iter_read(fbuf, &iter, &pos, 0);

		if (done < 0) {
			CERROR("%s: bulk %s at %llu+%zu failed: rc = %zd\n",
			       osd_name(osd), write ? "write" : "read",
			       lnb[i].lnb_file_offset, bytes, done);
			GOTO(out, rc = done);
		}

		if (write && done != (ssize_t)bytes)
			GOTO(out, rc = -EIO);

		/*
		 * A short read is a sparse range or EOF. Lustre expects zeroes
		 * and the full length in lnb_rc, so fill the remainder.
		 */
		{
			size_t filled = done;

			for (j = 0; j < n; j++) {
				size_t l = lnb[i + j].lnb_len;

				if (!write && filled < l) {
					void *k;

					k = kmap_local_page(lnb[i + j].lnb_page);
					memset(k + lnb[i + j].lnb_page_offset +
					       filled, 0, l - filled);
					kunmap_local(k);
					if (filled == 0)
						lnb[i + j].lnb_hole = 1;
				}
				filled -= min(filled, l);
				lnb[i + j].lnb_rc = l;
			}
		}

		i += n;
	}
	EXIT;
out:
	if (bvec != NULL)
		OBD_FREE_PTR_ARRAY_LARGE(bvec, npages);
	if (fbuf != NULL)
		fput(fbuf);
	return rc;
}

static int osd_read_prep(const struct lu_env *env, struct dt_object *dt,
			 struct niobuf_local *lnb, int npages)
{
	struct osd_object *obj = osd_dt_obj(dt);

	return osd_bulk_transfer(env, obj, lnb, npages, false);
}

/*
 * Write path. Nothing to prepare: the whole range is overwritten from the
 * network buffers.
 */
static int osd_write_prep(const struct lu_env *env, struct dt_object *dt,
			  struct niobuf_local *lnb, int npages)
{
	int i;

	for (i = 0; i < npages; i++)
		lnb[i].lnb_rc = lnb[i].lnb_len;

	return 0;
}

static int osd_declare_write_commit(const struct lu_env *env,
				    struct dt_object *dt,
				    struct niobuf_local *lnb, int npages,
				    struct thandle *handle)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct inode *inode = obj->oo_inode;
	long long space = 0;
	int i;

	ENTRY;

	if (inode == NULL)
		RETURN(-ENOENT);

	/*
	 * Declare the buffers that are not already allocated on disk. A
	 * rewrite of existing blocks consumes no new space.
	 *
	 * For a write, lnb_hole is the caller's statement that this range was
	 * not there before.
	 */
	for (i = 0; i < npages; i++) {
		if (lnb[i].lnb_rc == -ENOSPC || lnb[i].lnb_len == 0)
			continue;
		if (lnb[i].lnb_file_offset >= i_size_read(inode) ||
		    lnb[i].lnb_hole)
			space += lnb[i].lnb_len;
	}

	/*
	 * Declare the space, so the reservation can ask whether the
	 * filesystem has it at all: a transaction that has started must not
	 * be able to fail on it.
	 */
	osd_declare_op(handle, OSD_OT_WRITE,
		       (space + (1 << inode->i_blkbits) - 1) >>
		       inode->i_blkbits, 0);

	RETURN(0);
}

static int osd_write_commit(const struct lu_env *env, struct dt_object *dt,
			    struct niobuf_local *lnb, int npages,
			    struct thandle *handle, __u64 user_size)
{
	struct osd_object *obj = osd_dt_obj(dt);
	int i, rc;

	ENTRY;

	if (obj->oo_inode == NULL)
		RETURN(-ENOENT);
	for (i = 0; i < npages; i++)
		if (lnb[i].lnb_file_offset < 0 ||
		    lnb[i].lnb_len > MAX_LFS_FILESIZE - lnb[i].lnb_file_offset)
			RETURN(-EFBIG);

	/*
	 * Fault injection: pretend the allocator ran out of space. sanity 81b
	 * sets this to check that a write which cannot get blocks reports
	 * -ENOSPC to the client rather than succeeding. osd-ldiskfs honours it
	 * at the equivalent point in its own write path, so a backend that
	 * ignores it silently turns that test into a false pass.
	 */
	if (CFS_FAIL_CHECK(OBD_FAIL_OST_MAPBLK_ENOSPC))
		RETURN(-ENOSPC);
	osd_exec_op(env, handle, OSD_OT_WRITE, 0, 0);

	/*
	 * Track before attempting I/O: a short write or data-sync error can
	 * leave dirty pages even when the transfer fails. This must not
	 * depend on whole-request success.
	 */
	if (npages > 0)
		osd_trans_note_dirty(handle, obj->oo_inode);

	rc = osd_bulk_transfer(env, obj, lnb, npages, true);
	if (rc != 0)
		RETURN(rc);

	/*
	 * Timestamps are the OFD's business, not this function's.
	 *
	 * This used to stamp mtime and ctime unconditionally on every bulk
	 * write, on the grounds that an O_DIRECT overwrite of an allocated
	 * extent changes no XFS metadata by itself. That is true and it is
	 * still the wrong place to fix it: the OFD sends the client's
	 * timestamps down as an explicit attr_set, and overwriting them here
	 * afterwards discards a time the client deliberately chose. It broke
	 * sanity 39c, 39g and 39h, each of which sets an mtime with touch -m
	 * and requires it to survive a write, a lock cancel and a remount.
	 *
	 * osd-ldiskfs does not do this either: its write path only moves
	 * mtime when ldiskfs_update_inode_size() reports the size actually
	 * grew, and leaves it alone otherwise. Match that.
	 */
	if (user_size > 0 && user_size > i_size_read(obj->oo_inode)) {
		struct iattr attr = { .ia_valid = ATTR_SIZE | ATTR_FORCE };
		struct file *file;

		if (user_size > MAX_LFS_FILESIZE)
			RETURN(-EFBIG);
		attr.ia_size = user_size;
		file = osd_object_open(obj, O_RDWR);
		if (IS_ERR(file))
			RETURN(PTR_ERR(file));
		inode_lock(obj->oo_inode);
		if (user_size > i_size_read(obj->oo_inode))
			rc = notify_change(&nop_mnt_idmap, obj->oo_dentry,
					   &attr, NULL);
		inode_unlock(obj->oo_inode);
		if (!rc)
			rc = vfs_fsync(file, 1);
		fput(file);
		if (rc)
			RETURN(rc);
	}

	RETURN(0);
}

/* ------------------------------------------------------------------ */
/* truncate / punch                                                    */
/* ------------------------------------------------------------------ */

static int osd_declare_punch(const struct lu_env *env, struct dt_object *dt,
			     __u64 start, __u64 end, struct thandle *th)
{
	/*
	 * A punch frees space, but freeing is not always free: splitting an
	 * extent to remove a range from the middle needs a block for the new
	 * extent record. Declare one so a truncate cannot be the operation
	 * that runs out of space.
	 */
	osd_declare_op(th, OSD_OT_PUNCH, OSD_DECL_BLK_PUNCH, 0);

	return 0;
}

static int osd_punch(const struct lu_env *env, struct dt_object *dt,
		     __u64 start, __u64 end, struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_device *osd = osd_obj2dev(obj);
	int rc;

	ENTRY;

	if (obj->oo_inode == NULL || obj->oo_dentry == NULL)
		RETURN(-ENOENT);

	/*
	 * dbo_punch is only ever reached with end == OBD_OBJECT_EOF: the OFD
	 * only truncates. Refuse rather than truncate, because truncating
	 * would destroy data past @end.
	 */
	if (end != OBD_OBJECT_EOF) {
		CDEBUG(D_INODE,
		       "%s: range punch [%llu,%llu) does not arrive here\n",
		       osd_name(osd), start, end);
		RETURN(-EOPNOTSUPP);
	}
	osd_exec_op(env, th, OSD_OT_PUNCH, 0, 0);

	/*
	 * do_truncate() is not exported to modules and vfs_truncate() would
	 * redo permission checks on a path we already hold, so drive
	 * notify_change() directly. ATTR_FORCE skips the may_setattr() checks
	 * that make no sense for an OSD-internal object.
	 */
	{
		struct iattr ia = {
			.ia_valid = ATTR_SIZE | ATTR_FORCE,
			.ia_size  = start,
		};

		inode_lock(obj->oo_inode);
		rc = notify_change(&nop_mnt_idmap, obj->oo_dentry, &ia, NULL);
		inode_unlock(obj->oo_inode);
	}

	RETURN(rc);
}

static loff_t osd_lseek(const struct lu_env *env, struct dt_object *dt,
			loff_t offset, int whence)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct file *file;
	loff_t rc;

	file = osd_object_open(obj, O_RDONLY);
	if (IS_ERR(file))
		return PTR_ERR(file);

	rc = vfs_llseek(file, offset, whence);
	fput(file);

	return rc;
}

/*
 * Body operations for an object that does not exist yet.
 *
 * The declare phase runs before dt_create(), so Lustre legitimately calls
 * dt_declare_record_write() on an object with no inode.
 * dt_declare_record_write() asserts do_body_ops is non-NULL, so a
 * not-yet-created object still needs a vector -- just one that can only
 * declare. osd-ldiskfs does the same with osd_body_ops_new; without it the
 * first mount LBUGs.
 */
const struct dt_body_operations osd_body_ops_new = {
	.dbo_declare_write	  = osd_declare_write,
};

const struct dt_body_operations osd_body_ops = {
	.dbo_read		  = osd_read,
	.dbo_declare_write	  = osd_declare_write,
	.dbo_write		  = osd_write,
	.dbo_bufs_get		  = osd_bufs_get,
	.dbo_bufs_put		  = osd_bufs_put,
	.dbo_write_prep		  = osd_write_prep,
	.dbo_declare_write_commit = osd_declare_write_commit,
	.dbo_write_commit	  = osd_write_commit,
	.dbo_read_prep		  = osd_read_prep,
	.dbo_declare_punch	  = osd_declare_punch,
	.dbo_punch		  = osd_punch,
	.dbo_lseek		  = osd_lseek,
};
