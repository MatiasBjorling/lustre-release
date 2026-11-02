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
#include <linux/fiemap.h>
#include <linux/falloc.h>

#include <lustre_compat.h>

#include <lustre_quota.h>

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

/* ------------------------------------------------------------------ */
/* small record I/O: last_rcvd, LAST_ID, llog records                  */
/* ------------------------------------------------------------------ */

/*
 * LAST_ID: the per-sequence object id watermark, eight bytes rewritten in
 * place. As file data it was written back at the commit barrier, outside the
 * XFS log and so unordered against the creates it describes. It is kept in
 * one attribute instead, logged when the transaction that wrote it stops,
 * after that transaction's creates. Readers see the current value at once
 * from oo_rec_val; only the durable copy waits.
 *
 * A LAST_ID left by a crash between its create and its first value has no
 * attribute and size zero. Nothing can depend on a value that was never
 * logged, so it is removed at lookup and the caller creates it again, rather
 * than failing the mount reading eight bytes from an empty file.
 */
#define OSD_REC_SMALL_XATTR	"trusted.osdrec.value"

bool osd_rec_small(const struct osd_object *obj)
{
	return fid_is_last_id(lu_object_fid(&obj->oo_dt.do_lu));
}

/* Called once at lookup. Returns -ENOENT for a crash leftover it removed. */
int osd_rec_small_load(const struct lu_env *env, struct osd_object *obj)
{
	struct osd_device *osd = osd_obj2dev(obj);
	struct inode *inode = obj->oo_inode;
	loff_t size = i_size_read(inode);
	int rc;

	rc = __vfs_getxattr(obj->oo_dentry, inode, OSD_REC_SMALL_XATTR,
			    obj->oo_rec_val, sizeof(obj->oo_rec_val));
	if (rc >= 0) {
		obj->oo_rec_len = rc;
		obj->oo_rec_cached = true;
		return 0;
	}
	if (rc == -ERANGE)
		return -EUCLEAN;
	if (rc != -ENODATA)
		return rc;

	/* Written as file data by an earlier osd-xfs; adopted as is. */
	if (size > 0) {
		struct file *file = osd_object_open(obj, O_RDONLY);
		loff_t pos = 0;

		if (size > sizeof(obj->oo_rec_val))
			return -EUCLEAN;
		if (IS_ERR(file))
			return PTR_ERR(file);
		rc = kernel_read(file, obj->oo_rec_val, size, &pos);
		fput(file);
		if (rc != size)
			return rc < 0 ? rc : -EIO;
		obj->oo_rec_len = size;
		obj->oo_rec_cached = true;
		return 0;
	}

	CWARN("%s: removing "DFID", created without a value before a crash\n",
	      osd_name(osd), PFID(lu_object_fid(&obj->oo_dt.do_lu)));
	rc = osd_obj_map_delete(env, osd, lu_object_fid(&obj->oo_dt.do_lu));
	return rc ? rc : -ENOENT;
}

static int osd_rec_small_log(struct osd_object *obj)
{
	int rc;

	lockdep_assert_held(&obj->oo_rec_lock);
	if (!obj->oo_rec_dirty)
		return 0;
	if (CFS_FAIL_CHECK(OBD_FAIL_OSD_XFS_LASTID_LOG))
		return -EIO;
	inode_lock(obj->oo_inode);
	rc = __vfs_setxattr(&nop_mnt_idmap, obj->oo_dentry, obj->oo_inode,
			    OSD_REC_SMALL_XATTR, obj->oo_rec_val,
			    obj->oo_rec_len, 0);
	inode_unlock(obj->oo_inode);
	if (rc == 0)
		obj->oo_rec_dirty = false;
	return rc;
}

static ssize_t osd_rec_small_read(struct osd_object *obj, struct lu_buf *buf,
				  loff_t *pos)
{
	ssize_t rc = 0;

	mutex_lock(&obj->oo_rec_lock);
	if (*pos < 0)
		rc = -EINVAL;
	else if (*pos < obj->oo_rec_len) {
		rc = min_t(loff_t, buf->lb_len, obj->oo_rec_len - *pos);
		memcpy(buf->lb_buf, obj->oo_rec_val + *pos, rc);
		*pos += rc;
	}
	mutex_unlock(&obj->oo_rec_lock);
	return rc;
}

static ssize_t osd_rec_small_write(const struct lu_env *env,
				   struct osd_object *obj,
				   const struct lu_buf *buf, loff_t *pos,
				   struct thandle *handle)
{
	struct osd_thandle *oh = handle ? osd_thandle(handle) : NULL;
	ssize_t rc = buf->lb_len;
	int i;

	if (*pos < 0 || buf->lb_len > OSD_REC_SMALL_MAX ||
	    *pos > OSD_REC_SMALL_MAX - buf->lb_len)
		return -EFBIG;

	mutex_lock(&obj->oo_rec_lock);
	if (*pos > obj->oo_rec_len)
		memset(obj->oo_rec_val + obj->oo_rec_len, 0,
		       *pos - obj->oo_rec_len);
	memcpy(obj->oo_rec_val + *pos, buf->lb_buf, buf->lb_len);
	obj->oo_rec_len = max_t(loff_t, obj->oo_rec_len, *pos + buf->lb_len);
	obj->oo_rec_cached = true;
	obj->oo_rec_dirty = true;
	*pos += buf->lb_len;

	if (oh == NULL) {
		/* Nothing to defer it to: durable before returning. */
		int err = osd_rec_small_log(obj);

		mutex_unlock(&obj->oo_rec_lock);
		if (err)
			return osd_commit_error(osd_obj2dev(obj), err);
		return osd_force_log(osd_obj2dev(obj)) ?: rc;
	}
	mutex_unlock(&obj->oo_rec_lock);

	for (i = 0; i < oh->ot_ndefer; i++)
		if (oh->ot_defer[i] == obj)
			return rc;
	if (oh->ot_ndefer == OSD_TH_MAX_DEFER)
		return -E2BIG;
	lu_object_get(&obj->oo_dt.do_lu);
	oh->ot_defer[oh->ot_ndefer++] = obj;
	return rc;
}

/*
 * Log the small records this transaction wrote, after all of its effects.
 * Runs at trans_stop on every exit, so the references are always dropped.
 */
int osd_rec_defer_flush(const struct lu_env *env, struct osd_thandle *oh)
{
	int i, rc = 0;

	for (i = 0; i < oh->ot_ndefer; i++) {
		struct osd_object *obj = oh->ot_defer[i];
		int err;

		mutex_lock(&obj->oo_rec_lock);
		err = osd_rec_small_log(obj);
		mutex_unlock(&obj->oo_rec_lock);
		if (err && rc == 0)
			rc = err;
		dt_object_put(env, &obj->oo_dt);
		oh->ot_defer[i] = NULL;
	}
	oh->ot_ndefer = 0;
	return rc;
}
/*
 * last_rcvd is rewritten once per write RPC, always at the same offset. As
 * regular file data that is a 4 KiB page the commit path has to write back,
 * which on a rotating device is a seek out of the sequential data stream and
 * back: measured at a fifth of every request reaching the drive under a
 * streaming write.
 *
 * osd-ldiskfs does not pay that. It sets LDISKFS_INODE_JOURNAL_DATA and
 * writes the block through the journal, so the record is metadata and rides
 * the journal's own sequential stream. XFS has no data journalling, and
 * exports no symbols, so the only route from here to logged XFS metadata is
 * the attribute fork through __vfs_setxattr, which osd-xfs already uses.
 *
 * A local (in-line) attribute value is logged: a same-size shortform replace
 * is XFS_ILOG_ADATA inode-fork data, and a leaf-format update logs the
 * name/value range with xfs_trans_log_buf. Both are recovered from the log
 * with no block writeback, which is the ldiskfs property being matched. A
 * value must stay below xfs_attr_leaf_entsize_local_max(), 3 KiB at a 4 KiB
 * block size, or it goes remote and its blocks are written by a raw
 * xfs_bwrite that is never logged. Records are therefore stored in chunks.
 *
 * The file keeps its size and its data blocks: i_size is what
 * tgt_server_data_init() branches on and what bounds client-slot
 * enumeration, so it is still extended, and the file data is still written
 * so that lr_reader and a direct mount see plausible content. The data is a
 * mirror; the xattrs are authoritative on read.
 */
#define OSD_REC_CHUNK		512
#define OSD_REC_PREFIX		"trusted.osdrec."
#define OSD_REC_TRUNCATE "trusted.osdrec.truncate"

struct osd_rec_truncate {
	__le64 size;
	__le64 previous_size;
};

bool osd_rec_logged(const struct osd_object *obj)
{
	const struct lu_fid *fid = lu_object_fid(&obj->oo_dt.do_lu);

	return fid_seq(fid) == FID_SEQ_LOCAL_FILE &&
	       fid_oid(fid) == LAST_RECV_OID;
}

static void osd_rec_name(char *buf, size_t len, loff_t chunk)
{
	snprintf(buf, len, OSD_REC_PREFIX "%llu", (unsigned long long)chunk);
}

/*
 * Read or write one record through the attribute fork, one chunk at a time.
 *
 * Missing chunks may be seeded from file data only during legacy migration.
 * Once ready, every chunk below EOF is authoritative: missing or short values
 * are corruption, never an invitation to resurrect stale mirror bytes.
 */
static ssize_t osd_rec_xattr(struct osd_object *obj, struct file *file,
			     void *buf, size_t len, loff_t pos, bool write)
{
	struct inode *inode = obj->oo_inode;
	struct dentry *dentry = obj->oo_dentry;
	void *chunk;
	size_t done = 0;
	int rc = 0;

	if (inode == NULL)
		return -ENOENT;

	OBD_ALLOC(chunk, OSD_REC_CHUNK);
	if (chunk == NULL)
		return -ENOMEM;

	lockdep_assert_held(&obj->oo_rec_lock);

	while (done < len) {
		loff_t at = pos + done;
		loff_t base = round_down(at, OSD_REC_CHUNK);
		size_t off = at - base;
		size_t bytes = min_t(size_t, len - done, OSD_REC_CHUNK - off);
		char name[sizeof(OSD_REC_PREFIX) + 24];
		int got;

		osd_rec_name(name, sizeof(name), base / OSD_REC_CHUNK);

		got = __vfs_getxattr(dentry, inode, name, chunk,
				     OSD_REC_CHUNK);
		if (got == -ENODATA) {
			loff_t at_base = base;
			ssize_t n;

			if (obj->oo_rec_ready && base < i_size_read(inode)) {
				rc = -EUCLEAN;
				break;
			}
			n = kernel_read(file, chunk, OSD_REC_CHUNK, &at_base);
			if (n < 0) {
				rc = n;
				break;
			}
			if (n != min_t(loff_t, OSD_REC_CHUNK,
					max_t(loff_t, 0, i_size_read(inode) - base))) {
				rc = -EUCLEAN;
				break;
			}
			if (n < OSD_REC_CHUNK)
				memset(chunk + n, 0, OSD_REC_CHUNK - n);
		} else if (got < 0) {
			rc = got == -ERANGE ? -EUCLEAN : got;
			break;
		} else if (got != OSD_REC_CHUNK) {
			rc = -EUCLEAN;
			break;
		}

		if (write) {
			memcpy(chunk + off, buf + done, bytes);
			inode_lock(inode);
			rc = __vfs_setxattr(&nop_mnt_idmap, dentry, inode,
					    name, chunk, OSD_REC_CHUNK, 0);
			inode_unlock(inode);
			if (rc != 0)
				break;
		} else {
			memcpy(buf + done, chunk + off, bytes);
		}

		done += bytes;
	}

	OBD_FREE(chunk, OSD_REC_CHUNK);

	return rc != 0 ? rc : done;
}

/* Caller holds oo_rec_lock. A durable marker makes interrupted shrink replayable. */
static int osd_rec_finish_truncate(struct osd_object *obj, struct file *file)
{
	struct osd_rec_truncate marker;
	struct inode *inode = obj->oo_inode;
	struct iattr attr = { .ia_valid = ATTR_SIZE | ATTR_FORCE };
	u64 size, previous, base;
	char chunk[OSD_REC_CHUNK];
	int rc;

	lockdep_assert_held(&obj->oo_rec_lock);
	rc = __vfs_getxattr(obj->oo_dentry, inode, OSD_REC_TRUNCATE,
			  &marker, sizeof(marker));
	if (rc == -ENODATA)
		return 0;
	if (rc < 0)
		return rc;
	if (rc != sizeof(marker))
		return -EUCLEAN;
	size = le64_to_cpu(marker.size);
	previous = le64_to_cpu(marker.previous_size);
	if (size > previous || previous > MAX_LFS_FILESIZE)
		return -EUCLEAN;
	if (size < LR_SERVER_SIZE)
		return -EUCLEAN;
	for (base = round_down(size, OSD_REC_CHUNK); base < previous;
	     base += OSD_REC_CHUNK) {
		char name[sizeof(OSD_REC_PREFIX) + 24];
		size_t keep = base < size ? size - base : 0;

		memset(chunk, 0, sizeof(chunk));
		if (keep) {
			rc = osd_rec_xattr(obj, file, chunk, keep, base, false);
			if (rc < 0)
				return rc;
		}
		osd_rec_name(name, sizeof(name), base / OSD_REC_CHUNK);
		inode_lock(inode);
		rc = __vfs_setxattr(&nop_mnt_idmap, obj->oo_dentry, inode,
				    name, chunk, sizeof(chunk), 0);
		inode_unlock(inode);
		if (rc)
			return rc;
	}
	/* Persist zeroed tails before file size can conceal their former extent. */
	rc = vfs_fsync(file, 0);
	if (rc)
		return rc;
	attr.ia_size = size;
	inode_lock(inode);
	rc = notify_change(&nop_mnt_idmap, obj->oo_dentry, &attr, NULL);
	inode_unlock(inode);
	if (!rc)
		rc = vfs_fsync(file, 0);
	if (rc)
		return rc;
	inode_lock(inode);
	rc = __vfs_removexattr(&nop_mnt_idmap, obj->oo_dentry,
			      OSD_REC_TRUNCATE);
	inode_unlock(inode);
	return rc ? rc : vfs_fsync(file, 0);
}

static int osd_rec_format_set(struct osd_object *obj, struct file *file,
			      u32 state)
{
	struct osd_rec_format format = {
		.version = cpu_to_le32(OSD_REC_FORMAT_VERSION),
		.state = cpu_to_le32(state),
	};
	int rc;

	inode_lock(obj->oo_inode);
	rc = __vfs_setxattr(&nop_mnt_idmap, obj->oo_dentry, obj->oo_inode,
			    OSD_REC_FORMAT_XATTR, &format, sizeof(format), 0);
	inode_unlock(obj->oo_inode);
	return rc ? rc : vfs_fsync(file, 0);
}

/* No target reads are exposed until this migration/replay has completed. */
static int osd_rec_prepare(struct osd_object *obj, struct file *file,
			   bool readonly)
{
	struct osd_rec_format format;
	struct lr_server_data header;
	char *chunk = (char *)&header;
	struct inode *inode = obj->oo_inode;
	loff_t size, base, pos;
	bool ready = false, marked;
	int rc;

	lockdep_assert_held(&obj->oo_rec_lock);
	BUILD_BUG_ON(sizeof(header) != OSD_REC_CHUNK);
	rc = __vfs_getxattr(obj->oo_dentry, inode, OSD_REC_FORMAT_XATTR,
			  &format, sizeof(format));
	marked = rc != -ENODATA;
	if (rc != -ENODATA) {
		if (rc < 0)
			return rc == -ERANGE ? -EUCLEAN : rc;
		if (rc != sizeof(format))
			return -EUCLEAN;
		if (le32_to_cpu(format.version) != OSD_REC_FORMAT_VERSION)
			return -EOPNOTSUPP;
		if (le32_to_cpu(format.state) != OSD_REC_FORMAT_MIGRATING &&
		    le32_to_cpu(format.state) != OSD_REC_FORMAT_READY)
			return -EUCLEAN;
		ready = le32_to_cpu(format.state) == OSD_REC_FORMAT_READY;
	}
	/* A read-only opener must not replay either sort of repair marker. */
	if (readonly) {
		rc = __vfs_getxattr(obj->oo_dentry, inode, OSD_REC_TRUNCATE,
				  NULL, 0);
		if (rc != -ENODATA)
			return rc < 0 ? rc : -EROFS;
		if (!ready && (marked || i_size_read(inode)))
			return -EROFS;
	}
	obj->oo_rec_ready = ready;
	size = i_size_read(inode);
	/* New objects acquire their format with their first complete header. */
	if (!size)
		return ready ? -EUCLEAN : 0;
	if (size < sizeof(header))
		return -EUCLEAN;
	rc = osd_rec_xattr(obj, file, &header, sizeof(header), 0, false);
	if (rc < 0)
		return rc;
	if (!marked && (le32_to_cpu(header.lsd_feature_incompat) &
			OBD_INCOMPAT_XFS_REC))
		return -EUCLEAN;
	if (ready && !(le32_to_cpu(header.lsd_feature_incompat) &
		       OBD_INCOMPAT_XFS_REC))
		return -EUCLEAN;
	if (!ready) {
		__le32 incompat;

		rc = osd_rec_format_set(obj, file, OSD_REC_FORMAT_MIGRATING);
		if (rc)
			return rc;
		/* Fence the file reader before creating even attribute zero. A
		 * crash before this fence must not leave a newly seeded attribute
		 * shadowing later writes by an older file-only implementation.
		 * Preserve each legacy header's own bytes: an existing attr zero
		 * may already be authoritative and newer than the file header.
		 */
		pos = offsetof(struct lr_server_data, lsd_feature_incompat);
		rc = kernel_read(file, &incompat, sizeof(incompat), &pos);
		if (rc != sizeof(incompat))
			return rc < 0 ? rc : -EUCLEAN;
		incompat |= cpu_to_le32(OBD_INCOMPAT_XFS_REC);
		pos = offsetof(struct lr_server_data, lsd_feature_incompat);
		rc = kernel_write(file, &incompat, sizeof(incompat), &pos);
		if (rc != sizeof(incompat))
			return rc < 0 ? rc : -EIO;
		rc = vfs_fsync(file, 0);
		if (rc)
			return rc;
		/* Old osd-xfs reads attribute zero instead: fence it separately,
		 * retaining the previously authoritative header, then persist.
		 * Every MIGRATING resume repeats both fences before any seeding.
		 */
		header.lsd_feature_incompat |= cpu_to_le32(OBD_INCOMPAT_XFS_REC);
		rc = osd_rec_xattr(obj, file, &header, sizeof(header), 0, true);
		if (rc < 0)
			return rc;
		rc = vfs_fsync(file, 0);
		if (rc)
			return rc;
	}
	/* Truncate replay also writes authoritative chunks; it must not run
	 * until both downgrade readers have been fenced durably.
	 */
	if (!readonly) {
		rc = osd_rec_finish_truncate(obj, file);
		if (rc)
			return rc;
		size = i_size_read(inode);
	}
	/* Preserve old authoritative attributes, including those whose mirror
	 * never reached disk. Seed only absent chunks, with a zero-padded EOF.
	 * Repeating any prefix after a crash preserves exactly the same bytes.
	 */
	for (base = 0; base < size; base += OSD_REC_CHUNK) {
		size_t bytes = min_t(loff_t, OSD_REC_CHUNK, size - base);

		rc = osd_rec_xattr(obj, file, chunk, bytes, base, false);
		if (rc < 0)
			return rc;
		if (!ready) {
			memset(chunk + bytes, 0, OSD_REC_CHUNK - bytes);
			rc = osd_rec_xattr(obj, file, chunk, OSD_REC_CHUNK, base,
					   true);
			if (rc < 0)
				return rc;
		}
	}
	if (!ready) {
		rc = vfs_fsync(file, 0);
		if (rc)
			return rc;
		rc = osd_rec_format_set(obj, file, OSD_REC_FORMAT_READY);
		if (rc)
			return rc;
	} else {
		pos = 0;
		rc = kernel_read(file, &header, sizeof(header), &pos);
		if (rc != sizeof(header))
			return rc < 0 ? rc : -EUCLEAN;
		if (!(le32_to_cpu(header.lsd_feature_incompat) &
		      OBD_INCOMPAT_XFS_REC))
			return -EUCLEAN;
	}
	obj->oo_rec_ready = true;
	return 0;
}

/* Materialize zero-filled holes before making their size visible. */
static int osd_rec_extend(struct osd_object *obj, struct file *file, u64 size)
{
	char zero[OSD_REC_CHUNK] = {};
	u64 pos = i_size_read(obj->oo_inode);
	int rc;

	while (pos < size) {
		size_t bytes = min_t(u64, size - pos,
				     OSD_REC_CHUNK - pos % OSD_REC_CHUNK);

		rc = osd_rec_xattr(obj, file, zero, bytes, pos, true);
		if (rc < 0)
			return rc;
		pos += bytes;
	}
	return 0;
}

int osd_rec_resize(struct osd_object *obj, u64 size)
{
	struct file *file;
	struct inode *inode = obj->oo_inode;
	struct iattr attr = { .ia_valid = ATTR_SIZE | ATTR_FORCE };
	int rc;

	if (size > MAX_LFS_FILESIZE)
		return -EFBIG;
	if (size < LR_SERVER_SIZE)
		return -EINVAL;
	file = osd_object_open(obj, O_RDWR);
	if (IS_ERR(file))
		return PTR_ERR(file);
	mutex_lock(&obj->oo_rec_lock);
	rc = osd_rec_finish_truncate(obj, file);
	if (rc)
		goto out;
	if (size < i_size_read(inode)) {
		struct osd_rec_truncate marker = {
			.size = cpu_to_le64(size),
			.previous_size = cpu_to_le64(i_size_read(inode)),
		};

		inode_lock(inode);
		rc = __vfs_setxattr(&nop_mnt_idmap, obj->oo_dentry, inode,
				    OSD_REC_TRUNCATE, &marker, sizeof(marker), 0);
		inode_unlock(inode);
		if (!rc)
			rc = vfs_fsync(file, 0);
		if (!rc)
			rc = osd_rec_finish_truncate(obj, file);
	} else if (size > i_size_read(inode)) {
		rc = osd_rec_extend(obj, file, size);
		if (rc)
			goto out;
		attr.ia_size = size;
		inode_lock(inode);
		rc = notify_change(&nop_mnt_idmap, obj->oo_dentry, &attr, NULL);
		inode_unlock(inode);
		if (!rc)
			rc = vfs_fsync(file, 0);
	}
out:
	mutex_unlock(&obj->oo_rec_lock);
	fput(file);
	return rc ? osd_commit_error(osd_obj2dev(obj), rc) : 0;
}

int osd_rec_recover(struct osd_object *obj)
{
	bool readonly = osd_obj2dev(obj)->od_dt_dev.dd_rdonly ||
			sb_rdonly(obj->oo_inode->i_sb);
	struct file *file = osd_object_open(obj, readonly ? O_RDONLY : O_RDWR);
	int rc;

	if (IS_ERR(file))
		return PTR_ERR(file);
	mutex_lock(&obj->oo_rec_lock);
	rc = osd_rec_prepare(obj, file, readonly);
	mutex_unlock(&obj->oo_rec_lock);
	fput(file);
	if (rc)
		CERROR("%s: last_rcvd format initialization failed: rc=%d%s\n",
		       osd_name(osd_obj2dev(obj)), rc,
		       rc == -EROFS ? "; writable mount required for record repair" : "");
	return rc;
}

static ssize_t osd_read(const struct lu_env *env, struct dt_object *dt,
			struct lu_buf *buf, loff_t *pos)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct file *file;
	ssize_t rc;

	if (osd_rec_small(obj))
		return osd_rec_small_read(obj, buf, pos);

	file = osd_object_open(obj, O_RDONLY);
	if (IS_ERR(file))
		return PTR_ERR(file);

	if (osd_rec_logged(obj)) {
		loff_t size;

		mutex_lock(&obj->oo_rec_lock);
		size = i_size_read(obj->oo_inode);
		if (*pos < 0)
			rc = -EINVAL;
		else if (*pos >= size)
			rc = 0;
		else
			rc = osd_rec_xattr(obj, file, buf->lb_buf,
					   min_t(u64, buf->lb_len, size - *pos),
					   *pos, false);
		if (rc > 0)
			*pos += rc;
		mutex_unlock(&obj->oo_rec_lock);
	} else {
		rc = kernel_read(file, buf->lb_buf, buf->lb_len, pos);
	}
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
	if (osd_rec_small(obj))
		blocks = OSD_DECL_BLK_XATTR_SET;
	else if (osd_rec_logged(obj))
		blocks += (DIV_ROUND_UP_ULL(buf->lb_len, OSD_REC_CHUNK) + 1) *
			  OSD_DECL_BLK_XATTR_SET;
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
	if (osd_rec_small(obj))
		return osd_rec_small_write(env, obj, buf, pos, handle);

	/*
	 * Not O_DSYNC when a transaction is carrying the write. Every write
	 * RPC updates last_rcvd, so a per-write data-sync puts one 4 KiB
	 * write to the same block on the platter per RPC, which on a
	 * rotating device is a seek out of the sequential data stream and
	 * back. The transaction's commit path writes the file back instead,
	 * once per batch, before the callbacks that report those
	 * transactions durable, so the durability boundary is unchanged.
	 *
	 * Without a transaction there is nothing to carry the sync, so the
	 * write has to be durable when it returns.
	 */
	/*
	 * O_RDWR for a logged record: seeding a chunk that has no logged
	 * copy yet reads the file-data mirror first.
	 */
	file = osd_object_open(obj, (osd_rec_logged(obj) ? O_RDWR : O_WRONLY) |
				    (handle != NULL ? 0 : O_DSYNC));
	if (IS_ERR(file))
		return PTR_ERR(file);

	/*
	 * The record goes to the attribute fork, where XFS logs it, and then
	 * to file data. The file data keeps i_size right, which is what
	 * tgt_server_data_init() branches on and what bounds client-slot
	 * enumeration, and keeps a direct mount or lr_reader able to see the
	 * record. It is only a mirror: the read path above prefers the
	 * logged copy.
	 *
	 * Once the format is READY, the mirror is never used to recover missing
	 * attributes. Its header permanently retains the incompatibility fence;
	 * slot enumeration continues to use the durable file size.
	 */
	if (osd_rec_logged(obj)) {
		loff_t start = *pos;
		loff_t old_size;
		const __le32 fence = cpu_to_le32(OBD_INCOMPAT_XFS_REC);
		const size_t fence_at = offsetof(struct lr_server_data,
					       lsd_feature_incompat);
		size_t i;

		mutex_lock(&obj->oo_rec_lock);
		rc = osd_rec_finish_truncate(obj, file);
		if (rc)
			goto out_record_error;
		old_size = i_size_read(obj->oo_inode);
		if (start < 0 || buf->lb_len > MAX_LFS_FILESIZE - start) {
			rc = -EFBIG;
			goto out_record;
		}
		if (!obj->oo_rec_ready) {
			/* A newly created object has no header to migrate yet. */
			if (old_size || start || buf->lb_len != LR_SERVER_SIZE) {
				rc = -EINVAL;
				goto out_record;
			}
		}
		/* Never allow a record writer to remove the downgrade fence. */
		for (i = 0; i < sizeof(fence); i++) {
			u8 mask = ((const u8 *)&fence)[i];

			if (mask && start <= fence_at + i &&
			    start + buf->lb_len > fence_at + i &&
			    (((u8 *)buf->lb_buf)[fence_at + i - start] & mask) != mask) {
				rc = -EINVAL;
				goto out_record;
			}
		}
		if (!obj->oo_rec_ready) {
			rc = osd_rec_format_set(obj, file, OSD_REC_FORMAT_MIGRATING);
			if (rc)
				goto out_record_error;
			rc = kernel_write(file, buf->lb_buf, buf->lb_len, pos);
			if (rc >= 0 && rc != buf->lb_len)
				rc = -EIO;
			if (rc >= 0) {
				int err = osd_rec_prepare(obj, file, false);

				if (err)
					rc = err;
			}
			goto out_record_error;
		}
		if (start > old_size) {
			rc = osd_rec_extend(obj, file, start);
			if (rc)
				goto out_record_error;
		}
		rc = osd_rec_xattr(obj, file, buf->lb_buf, buf->lb_len,
				   start, true);
		if (rc < 0)
			goto out_record_error;
		rc = kernel_write(file, buf->lb_buf, buf->lb_len, pos);
		if (rc >= 0 && rc != buf->lb_len)
			rc = -EIO;
		/* Recovery enumerates slots by durable file size, not xattr keys. */
		if (rc >= 0 && (start + buf->lb_len > old_size || !handle)) {
			int err = vfs_fsync(file, 1);

			if (err)
				rc = err;
		}
out_record_error:
		mutex_unlock(&obj->oo_rec_lock);
		if (rc < 0)
			osd_commit_error(osd_obj2dev(obj), rc);
		fput(file);
		return rc;
out_record:
		mutex_unlock(&obj->oo_rec_lock);
	} else {
		osd_trans_note_dirty(handle, obj->oo_inode);
		rc = kernel_write(file, buf->lb_buf, buf->lb_len, pos);
	}
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

/*
 * Cache hit accounting for a bulk read.
 *
 * The names have to match osd-ldiskfs: sanity 151 and 156 read cache_access
 * and cache_hit through roc_hit() in the test framework and abort with
 * "roc_hit is not safe to use" if either is missing. Counted for reads only,
 * because a write is not a cache access.
 *
 * Called with the number of pages in the request and how many of them the
 * object's address space already held.
 */
static void osd_cache_account(struct osd_device *osd, int npages, int hits)
{
	if (osd->od_stats == NULL || npages <= 0)
		return;
	if (osd->od_read_cache == 0 && osd->od_writethrough_cache == 0)
		return;

	if (hits != 0)
		lprocfs_counter_add(osd->od_stats, LPROC_OSD_XFS_CACHE_HIT,
				    hits);
	if (npages - hits != 0)
		lprocfs_counter_add(osd->od_stats, LPROC_OSD_XFS_CACHE_MISS,
				    npages - hits);
	lprocfs_counter_add(osd->od_stats, LPROC_OSD_XFS_CACHE_ACCESS, npages);
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
 * Writes use O_DIRECT for aligned runs when writethrough caching is off,
 * falling back to buffered I/O when necessary. Both paths use RWF_DSYNC:
 * generic_write_sync() invokes XFS fsync, which handles data-device cache
 * flushes even with an external log or when forcing the log is a no-op.
 * Direct I/O by itself provides no such durability guarantee.
 *
 * Reads use the filesystem page cache unless both cache switches are off.
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

static bool osd_bulk_aligned(struct osd_object *obj, loff_t off, size_t len)
{
	unsigned int bs = i_blocksize(obj->oo_inode);

	return IS_ALIGNED(off, bs) && IS_ALIGNED(len, bs);
}

static int osd_bulk_transfer(const struct lu_env *env, struct osd_object *obj,
			     struct niobuf_local *lnb, int npages, bool write)
{
	struct osd_device *osd = osd_obj2dev(obj);
	struct file *fdir = NULL, *fbuf = NULL;
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

	/*
	 * O_DIRECT may be refused by the backing filesystem; that is not fatal,
	 * it just means every run takes the buffered path.
	 *
	 * Writes go direct only when writethrough caching is off. With it on,
	 * the switch means what it says in osd-ldiskfs: the write goes through
	 * the page cache and leaves the data there, so a client that writes a
	 * file and reads it straight back is served from OSS memory instead of
	 * the platter. Writing with O_DIRECT regardless leaves the object's
	 * address space empty after a write, every subsequent read misses, and
	 * sanity 151 and 156 fail with "NOT IN CACHE" on exactly that
	 * sequence. RWF_DSYNC makes buffered and direct writes durable before
	 * reporting success.
	 *
	 * Reads normally stay buffered on purpose, because the OSS page cache
	 * and its readahead are worth more than the copy they cost. The
	 * exception is when both cache switches are off: that is an explicit
	 * request not to use the page cache at all, and osd-ldiskfs honours it
	 * by bypassing the cache entirely. Serving such a read from cache
	 * anyway is what sanity 156 catches on its final case.
	 */
	if (write) {
		if (osd->od_writethrough_cache == 0) {
			fdir = osd_object_open(obj, O_WRONLY | O_DIRECT);
			if (IS_ERR(fdir))
				fdir = NULL;
		}
	} else if (osd->od_read_cache == 0 && osd->od_writethrough_cache == 0) {
		fdir = osd_object_open(obj, O_RDONLY | O_DIRECT);
		if (IS_ERR(fdir))
			fdir = NULL;
	}

	fbuf = osd_object_open(obj, write ? O_WRONLY : O_RDONLY);
	if (IS_ERR(fbuf)) {
		rc = PTR_ERR(fbuf);
		fbuf = NULL;
		GOTO(out, rc);
	}

	isize = i_size_read(obj->oo_inode);

	for (i = 0; i < npages; ) {
		struct iov_iter iter;
		struct file *file;
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

		file = (fdir != NULL && osd_bulk_aligned(obj, pos, bytes)) ?
			fdir : fbuf;

		iov_iter_bvec(&iter, write ? ITER_SOURCE : ITER_DEST,
			      bvec, n, bytes);

		if (write)
			done = vfs_iter_write(file, &iter, &pos, RWF_DSYNC);
		else
			done = vfs_iter_read(file, &iter, &pos, 0);

		if (done == -EINVAL && file == fdir &&
		    pos == lnb[i].lnb_file_offset &&
		    iov_iter_count(&iter) == bytes) {
			/* Retry alignment rejection, never a post-I/O sync error. */
			pos = lnb[i].lnb_file_offset;
			iov_iter_bvec(&iter, write ? ITER_SOURCE : ITER_DEST,
				      bvec, n, bytes);
			done = write ? vfs_iter_write(fbuf, &iter, &pos, RWF_DSYNC)
				     : vfs_iter_read(fbuf, &iter, &pos, 0);
		}

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
	if (fdir != NULL)
		fput(fdir);
	if (fbuf != NULL)
		fput(fbuf);
	return rc;
}

static int osd_read_prep(const struct lu_env *env, struct dt_object *dt,
			 struct niobuf_local *lnb, int npages)
{
	struct osd_object *obj = osd_dt_obj(dt);

	/*
	 * The file layer read: the page cache lookup happens inside
	 * vfs_iter_read() and leaves nothing per-page to inspect afterwards,
	 * so ask the mapping first. Same question, one step earlier.
	 */
	if (obj->oo_inode != NULL) {
		int hits = 0, i;

		for (i = 0; i < npages; i++) {
			struct folio *f;

			f = filemap_get_folio(obj->oo_inode->i_mapping,
					      lnb[i].lnb_file_offset >>
					      PAGE_SHIFT);
			if (IS_ERR_OR_NULL(f))
				continue;
			if (folio_test_uptodate(f))
				hits++;
			folio_put(f);
		}
		osd_cache_account(osd_obj2dev(obj), npages, hits);
	}
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
	struct osd_thandle *oh = osd_thandle(handle);
	enum osd_quota_local_flags local_flags = 0;
	long long space = 0;
	int i, rc;

	ENTRY;

	if (inode == NULL)
		RETURN(-ENOENT);

	/*
	 * Charge the buffers that are not already allocated on disk. A
	 * rewrite of existing blocks consumes no new space, and charging it
	 * would make a user's usage climb without their file growing.
	 *
	 * For a write, lnb_hole is the caller's statement that this range was
	 * not there before.
	 */
	for (i = 0; i < npages; i++) {
		if (lnb[i].lnb_rc == -ENOSPC || lnb[i].lnb_len == 0)
			continue;
		if (!(lnb[i].lnb_flags & OBD_BRW_OVER_USRQUOTA) &&
		    lnb[i].lnb_file_offset >= i_size_read(inode))
			space += lnb[i].lnb_len;
		else if (lnb[i].lnb_hole)
			space += lnb[i].lnb_len;
	}

	/*
	 * Declare the space itself, not just the quota. These are different
	 * questions: quota asks whether this user may consume it, the
	 * reservation asks whether the filesystem has it at all, and a
	 * transaction that has started must not be able to fail on either.
	 */
	osd_declare_op(handle, OSD_OT_WRITE,
		       (space + (1 << inode->i_blkbits) - 1) >>
		       inode->i_blkbits, 0);

	if (space == 0)
		RETURN(0);

	/*
	 * Quota is accounted in 1 KiB units, which is what lquota expects for
	 * a block resource.
	 */
	space = stoqb(space);

	/* stale flags from a previous use of these buffers */
	lnb[0].lnb_flags &= ~OBD_BRW_OVER_ALLQUOTA;

	rc = osd_declare_inode_qid(env, i_uid_read(inode), i_gid_read(inode),
				   osd_get_projid(inode), space, oh, obj,
				   &local_flags, true);

	/*
	 * The flags go on lnb[0] only, which is where the reply carries them
	 * back to the client, and the same place osd-ldiskfs puts them.
	 */
	if (local_flags & QUOTA_FL_OVER_USRQUOTA)
		lnb[0].lnb_flags |= OBD_BRW_OVER_USRQUOTA;
	if (local_flags & QUOTA_FL_OVER_GRPQUOTA)
		lnb[0].lnb_flags |= OBD_BRW_OVER_GRPQUOTA;
	if (local_flags & QUOTA_FL_OVER_PRJQUOTA)
		lnb[0].lnb_flags |= OBD_BRW_OVER_PRJQUOTA;
	if (local_flags & QUOTA_FL_ROOT_PRJQUOTA)
		lnb[0].lnb_flags |= OBD_BRW_ROOT_PRJQUOTA;

	/*
	 * -EDQUOT must be returned, not swallowed. An earlier version turned
	 * it into 0 on the theory that the flags alone would tell the client,
	 * which left the slave correctly refusing the reservation while the
	 * write went ahead anyway: 128 MiB stored against a 32 MiB limit, with
	 * the accounting reporting it accurately the whole time. The refusal
	 * has to reach ofd_commitrw_write() for the RPC to fail.
	 */
	RETURN(rc);
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
	 * Track before attempting I/O: an unaligned direct run can fall back
	 * to buffered, and a short write or data-sync error can leave dirty
	 * pages even when the transfer fails. This must not depend on the
	 * writethrough cache switch or whole-request success.
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
	 * dbo_punch is only ever reached with end == OBD_OBJECT_EOF: a range
	 * punch arrives from ofd_fallocate_hdl() as a fallocate carrying
	 * FALLOC_FL_PUNCH_HOLE, which osd_fallocate() handles. Refuse rather
	 * than truncate, because truncating would destroy data past @end.
	 */
	if (end != OBD_OBJECT_EOF) {
		CDEBUG(D_INODE,
		       "%s: range punch [%llu,%llu) does not arrive here\n",
		       osd_name(osd), start, end);
		RETURN(-EOPNOTSUPP);
	}
	osd_exec_op(env, th, OSD_OT_PUNCH, 0, 0);
	if (osd_rec_small(obj))
		RETURN(start == 0 ? 0 : -EOPNOTSUPP);
	if (osd_rec_logged(obj))
		RETURN(osd_rec_resize(obj, start));

	/*
	 * Content encrypted on the client is enciphered in
	 * LUSTRE_ENCRYPTION_UNIT_SIZE (4096 byte) units, each with an IV
	 * derived from its logical offset. Truncating to an offset inside a
	 * unit leaves a partial unit that cannot be deciphered, and the client
	 * reads back garbage for it -- silently, since nothing on this side
	 * can tell plaintext from nonsense.
	 *
	 * So round the truncation point up to the next unit boundary. Keeping
	 * a few extra bytes is harmless; the client knows the real size.
	 * osd-zfs does the same in its punch.
	 */
	if (obj->oo_lma_flags & LMAI_ENCRYPT &&
	    start & ~LUSTRE_ENCRYPTION_MASK) {
		__u64 aligned = (start & LUSTRE_ENCRYPTION_MASK) +
				LUSTRE_ENCRYPTION_UNIT_SIZE;

		CDEBUG(D_INODE,
		       "%s: encrypted object, rounding truncate %llu -> %llu\n",
		       osd_name(osd), start, aligned);
		start = aligned;
	}

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


/* ------------------------------------------------------------------ */
/* fiemap, fallocate, ladvise                                          */
/* ------------------------------------------------------------------ */

/* same bound osd-ldiskfs uses: the array must be addressable */
#define OSD_FIEMAP_MAX_EXTENTS	(UINT_MAX / sizeof(struct fiemap_extent))

/*
 * Report the object's extent map to a client asking through `lfs fiemap` or
 * filefrag(1).
 *
 * The caller's buffer is in kernel memory, which is the whole reason
 * vfs_fiemap_kernel() exists: fiemap_fill_next_extent() writes each extent
 * with copy_to_user(), so calling ->fiemap directly from here returns -EFAULT.
 * osd-ldiskfs solves the same problem with an ext4-specific patch that teaches
 * its fiemap to accept a kernel buffer.
 */
static int osd_fiemap_get(const struct lu_env *env, struct dt_object *dt,
			  struct fiemap *fm)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct inode *inode = obj->oo_inode;
	unsigned int mapped = 0;
	u64 maxbytes, len;
	int rc;

	ENTRY;

	if (inode == NULL)
		RETURN(-ENOENT);
	if (fm->fm_extent_count > OSD_FIEMAP_MAX_EXTENTS)
		RETURN(-EINVAL);

	/*
	 * fiemap_check_ranges() is not exported, so the same clamping is done
	 * here: refuse a start beyond the addressable range and trim a length
	 * that would run past it.
	 */
	maxbytes = inode->i_sb->s_maxbytes;
	if (fm->fm_start > maxbytes)
		RETURN(-EFBIG);
	len = fm->fm_length;
	if (len == 0)
		RETURN(-EINVAL);
	if (len > maxbytes || maxbytes - len < fm->fm_start)
		len = maxbytes - fm->fm_start;

	/*
	 * fm_extent_count == 0 is the caller asking only how many extents
	 * there are. vfs_fiemap_kernel() rejects a zero-sized array, so answer
	 * it by mapping into no space at all, which is what the ioctl path
	 * does.
	 */
	if (fm->fm_extent_count == 0) {
		struct fiemap_extent_info fieinfo = {
			.fi_flags	 = fm->fm_flags,
			.fi_extents_max	 = 0,
		};

		if (inode->i_op->fiemap == NULL)
			RETURN(-EOPNOTSUPP);
		rc = inode->i_op->fiemap(inode, &fieinfo, fm->fm_start, len);
		if (rc && rc != -EAGAIN)
			RETURN(rc);
		fm->fm_mapped_extents = fieinfo.fi_extents_mapped;
		RETURN(0);
	}

	rc = vfs_fiemap_kernel(inode, &fm->fm_extents[0], fm->fm_extent_count,
			       fm->fm_start, len, fm->fm_flags, &mapped);
	if (rc != 0)
		RETURN(rc);

	fm->fm_mapped_extents = mapped;

	RETURN(0);
}

/*
 * Preallocate space for an object, from fallocate(2) on a Lustre file.
 */
static int osd_declare_fallocate(const struct lu_env *env,
				 struct dt_object *dt, struct lu_attr *attr,
				 __u64 start, __u64 end, int mode,
				 struct thandle *th,
				 enum dt_fallocate_error_t *error_code)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_thandle *oh = osd_thandle(th);
	long long space;
	int rc;

	ENTRY;

	if (obj->oo_inode == NULL)
		RETURN(-ENOENT);

	/*
	 * FALLOC_FL_KEEP_SIZE only reserves blocks; anything else moves the
	 * size too, which is a separate attr_set. PUNCH_HOLE frees blocks
	 * rather than allocating them, so it is declared like a punch: no
	 * quota is charged, but an extent split can still need a block.
	 */
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		RETURN(-EOPNOTSUPP);

	if (mode & FALLOC_FL_PUNCH_HOLE) {
		osd_declare_op(th, OSD_OT_PUNCH, OSD_DECL_BLK_PUNCH, 0);
		RETURN(0);
	}

	if (end <= start)
		RETURN(0);
	if (end > MAX_LFS_FILESIZE)
		RETURN(-EFBIG);
	osd_declare_op(th, OSD_OT_WRITE,
		       DIV_ROUND_UP_ULL(end - start, i_blocksize(obj->oo_inode)), 0);

	/*
	 * Charge the whole range against quota. Being generous is the right
	 * error: a preallocation that is refused is better than one that
	 * succeeds and then cannot be written into.
	 */
	space = stoqb(end - start);
	rc = osd_declare_inode_qid(env, i_uid_read(obj->oo_inode),
				   i_gid_read(obj->oo_inode),
				   osd_get_projid(obj->oo_inode), space, oh,
				   obj, NULL, true);
	RETURN(rc);
}

static int osd_fallocate(const struct lu_env *env, struct dt_object *dt,
			 __u64 *start, __u64 end, int mode,
			 struct thandle *th)
{
	struct osd_object *obj = osd_dt_obj(dt);
	struct osd_device *osd = osd_obj2dev(obj);
	struct file *file;
	int rc;

	ENTRY;

	if (obj->oo_inode == NULL || obj->oo_dentry == NULL)
		RETURN(-ENOENT);

	/*
	 * Preallocation and hole punching, which is how a range punch reaches
	 * this backend: ofd_fallocate_hdl() turns it into a fallocate with
	 * FALLOC_FL_PUNCH_HOLE, never into a dbo_punch call. Anything else
	 * moves the size too, which is a separate attr_set.
	 *
	 * Encrypted objects are refused for a punch rather than rounded.
	 * Rounding is safe for a truncate because it keeps more data than
	 * asked; for a hole it would have to shrink the punched range to avoid
	 * destroying a partial encryption unit, and a punch that silently does
	 * less than it says is worse than one that declines.
	 */
	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
		RETURN(-EOPNOTSUPP);
	/*
	 * -1 disables preallocation; see fallocate_zero_blocks. It is set on a
	 * zoned data device, which cannot preallocate but can punch.
	 */
	if (osd->od_fallocate_zero_blocks < 0 && !(mode & FALLOC_FL_PUNCH_HOLE))
		RETURN(-EOPNOTSUPP);
	if (mode & FALLOC_FL_PUNCH_HOLE) {
		if (!(mode & FALLOC_FL_KEEP_SIZE))
			RETURN(-EOPNOTSUPP);
		if (obj->oo_lma_flags & LMAI_ENCRYPT &&
		    ((*start | end) & ~LUSTRE_ENCRYPTION_MASK)) {
			CDEBUG(D_INODE,
			       "%s: refusing unaligned punch [%llu,%llu) on an encrypted object\n",
			       osd_name(osd), *start, end);
			RETURN(-EOPNOTSUPP);
		}
	}
	if (end <= *start)
		RETURN(0);

	/*
	 * vfs_fallocate() wants a struct file, so open the object's own
	 * dentry. This is the same short-lived open the bulk write path uses;
	 * O_NOATIME because Lustre owns the timestamps of its objects.
	 */
	file = dentry_open(&(struct path){ .mnt = osd->od_mnt,
					   .dentry = obj->oo_dentry },
			   O_RDWR | O_NOATIME | O_LARGEFILE, current_cred());
	if (IS_ERR(file))
		RETURN(PTR_ERR(file));

	osd_exec_op(env, th, mode & FALLOC_FL_PUNCH_HOLE ?
		    OSD_OT_PUNCH : OSD_OT_WRITE, 0, 0);
	rc = vfs_fallocate(file, mode, *start, end - *start);
	fput(file);

	if (rc == 0)
		*start = end;

	RETURN(rc);
}

/*
 * `lfs ladvise`. Only DONTNEED is acted on, which is also all osd-ldiskfs
 * does: WILLREAD is served by the OFD's own readahead, and the lock hints are
 * handled above the OSD entirely.
 */
static int osd_ladvise(const struct lu_env *env, struct dt_object *dt,
		       __u64 start, __u64 end, enum lu_ladvise_type advice)
{
	struct osd_object *obj = osd_dt_obj(dt);
	int rc = 0;

	ENTRY;

	if (obj->oo_inode == NULL)
		RETURN(-ENOENT);

	switch (advice) {
	case LU_LADVISE_DONTNEED:
		if (end)
			invalidate_mapping_pages(obj->oo_inode->i_mapping,
						 start >> PAGE_SHIFT,
						 (end - 1) >> PAGE_SHIFT);
		break;
	default:
		rc = -ENOTSUPP;
		break;
	}

	RETURN(rc);
}

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
	.dbo_fiemap_get		  = osd_fiemap_get,
	.dbo_declare_fallocate	  = osd_declare_fallocate,
	.dbo_fallocate		  = osd_fallocate,
	.dbo_ladvise		  = osd_ladvise,
};
