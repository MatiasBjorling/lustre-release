/* SPDX-License-Identifier: GPL-2.0 */

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Internal definitions for the XFS OSD (osd-xfs).
 *
 * osd-xfs backs Lustre OSTs with an unmodified in-kernel XFS filesystem.
 *
 * Two findings from the design study shape this module, and both are worth
 * restating here because they are why the OST path is tractable at all:
 *
 *  1. On an OST, Lustre never looks an object up through a b-tree object
 *     index. osd_oi_lookup() in osd-ldiskfs dispatches every OST-side FID to
 *     the O/<seq>/d<N>/<oid> *directory* hierarchy, and the directory entries
 *     it creates carry no FID payload. So osd-xfs needs no IAM port and, more
 *     importantly, no change to the XFS on-disk directory format.
 *
 *  2. The only xattrs an OST object carries are trusted.lma (24 bytes) and
 *     trusted.fid (at most 56). Formatted with -i size=1024 they both live in
 *     the shortform attribute fork, so XFS never has to spill to a remote
 *     attribute block and never has to roll a transaction to set one.
 *
 * Transactions. Crash consistency comes from the XFS log, in two parts:
 *
 *  - An object is created with its identity in one XFS transaction:
 *    xfs_osd_create() logs the inode, its directory entry, trusted.lma and
 *    its initial mode, owner and times together. attr_set is likewise one
 *    logged transaction (xfs_osd_setattr()).
 *
 *  - Everything else is ordered, not grouped. XFS log recovery replays a
 *    prefix of committed transactions, so a transaction committed later is
 *    never recovered without the ones before it. Each dt operation commits
 *    its own XFS transactions in program order, data is made durable before
 *    the transaction that publishes it, and the recovery records (last_rcvd,
 *    LAST_ID) are logged xattrs written last, at trans_stop. A crash can
 *    therefore leave effects without their record, which Lustre replays, but
 *    never a record without its effects.
 *
 * See analysis/06-compound-transactions.md in the project for the audit.
 */

#ifndef _OSD_XFS_INTERNAL_H
#define _OSD_XFS_INTERNAL_H

#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/rwsem.h>
#include <linux/statfs.h>
#include <linux/xattr.h>

#include <lustre_compat/linux/fs.h>

#include <dt_object.h>
#include <lustre_disk.h>
#include <lustre_fid.h>
#include <obd.h>
#include <obd_class.h>

#include <linux/xfs_osd.h>

#define OSD_XFS_NAME		"osd-xfs"

/* backing filesystem type we vfs_kern_mount() */
#define OSD_XFS_FSTYPE		"xfs"

/*
 * Number of d<N> subdirectories under each O/<seq>/. Must be a power of two:
 * the object id is masked, not divided. Matches the ldiskfs default so that a
 * target laid out by either OSD looks the same to an administrator.
 */
#define OSD_XFS_SUBDIR_COUNT	32

/*
 * Commit accumulator batching.
 *
 * A purely time-based delay is the wrong shape. At 20 ms it cost 15% of
 * streaming write throughput and pushed p99 from 58 ms to 198 ms at qd64,
 * because Lustre clients need commit callbacks to release grant: delaying the
 * callback throttles the write pipeline even though the data is already on
 * disk.
 *
 * So: a timer to catch a trickle, plus an immediate force once enough
 * transactions have piled up.
 *
 * A write RPC without OBD_BRW_ASYNC sets th_sync, so under a streaming write
 * almost every transaction is one whose caller is blocked waiting for it.
 * Those join the batch and wait, so the timer is a latency floor for the RPC
 * as well as the window the batch accumulates in, and it has a peak rather
 * than being monotonic.
 *
 * Measured on six SMR-class SATA OSTs, 1 MiB sequential writes, six jobs at
 * queue depth 16:
 *
 *	1 ms	533 MiB/s	3.19 transactions per force
 *	4 ms	534 MiB/s	3.23
 *	10 ms	540 MiB/s	3.95
 *	25 ms	560 MiB/s	5.98
 *	50 ms	548 MiB/s	8.60
 *	100 ms	476 MiB/s	13.01
 *	150 ms	407 MiB/s	15.71
 *
 * Past 25 ms the added RPC latency costs more than the larger batch saves.
 * Tunable per target through commit_delay_ms, because the peak depends on
 * the device and the workload.
 */
#define OSD_COMMIT_DELAY_MS	25
#define OSD_COMMIT_BATCH_MAX	32

/* how many distinct quota ids one transaction may touch */
#define OSD_MAX_UGID_CNT	32

/*
 * Declared cost of each operation, in filesystem blocks and new inodes.
 *
 * These are deliberate over-estimates. The declare phase exists so a
 * transaction that has started cannot fail partway through for want of space,
 * and refusing work that would have fitted is a far better error than
 * discovering the problem in the middle of a change that cannot be rolled
 * back. osd-ldiskfs takes the same view with journal credits and says so.
 *
 * Inodes are counted separately because this backend builds its indexes out of
 * symlinks, so inserting a key allocates an inode rather than just filling a
 * directory block.
 */
#define OSD_DECL_BLK_ATTR_SET	0	/* fits in the inode */
#define OSD_DECL_BLK_XATTR_SET	2	/* may spill to a remote attr block */
#define OSD_DECL_BLK_CREATE	2	/* dirent, plus a possible btree split */
#define OSD_DECL_INO_CREATE	1
#define OSD_DECL_BLK_INSERT	2	/* symlink body, plus the dirent */
#define OSD_DECL_INO_INSERT	1
#define OSD_DECL_BLK_PUNCH	1	/* freeing can still split an extent */

/* the object index directory, immediately below the filesystem root */
#define OSD_OI_DIRNAME		"oi"

/*
 * Caps for the key/value index flavour. A key hex-encodes into a filename
 * (NAME_MAX is 255, so 64 bytes is comfortable) and a record hex-encodes into
 * a symlink body.
 */
#define OSD_KV_KEY_MAX		64
#define OSD_KV_REC_MAX		128

/*
 * XFS limits we need but which live in fs/xfs private headers, so they are not
 * reachable from a module. Kept here with the upstream source of each value so
 * they can be checked when the kernel moves.
 *
 *   XFS_MAXLINK           fs/xfs/libxfs/xfs_format.h
 *   XFS_XATTR_SIZE_MAX    fs/xfs/libxfs/xfs_format.h  (1 << 16)
 */
#ifndef XFS_MAXLINK
#define XFS_MAXLINK		((1U << 31) - 1U)
#endif
#ifndef XFS_XATTR_SIZE_MAX
#define XFS_XATTR_SIZE_MAX	(1 << 16)
#endif

/*
 * Largest single object size advertised to clients.
 *
 * XFS's own s_maxbytes is 8 EiB, the format limit. Advertising that is wrong
 * twice over: a 2-stripe write lands within a byte of MAX_LFS_FILESIZE and
 * fails with EFBIG, and clients take the figure literally.
 *
 * The value has to match what osd-ldiskfs reports, not merely be close to
 * it. ext4_max_size() gives 2^44 minus one filesystem block, and a client
 * writing a file striped across both backends uses the smallest limit among
 * its OSTs: advertising the round 2^44 made sanity 44c seek one block past
 * what the ldiskfs OSTs accept, and the write failed with EINVAL on
 * whichever OST the round robin happened to pick.
 */
#define OSD_XFS_MAX_OBJECT_BYTES(blocksize)	\
	((((loff_t)1 << 44) - (blocksize)))

struct osd_device;

/* ------------------------------------------------------------------ */

struct osd_object {
	struct dt_object	 oo_dt;

	/* pinned for the lifetime of the osd_object */
	struct inode		*oo_inode;
	/*
	 * Dentry for the object's entry in the O/ tree. Objects reached only
	 * by FID (never linked into a directory) leave this NULL.
	 */
	struct dentry		*oo_dentry;

	struct rw_semaphore	 oo_guard;
	/* serialises attribute and xattr updates against readers */
	struct rw_semaphore	 oo_ext_guard;
	/* Serializes logged-record RMW, mirror updates, and truncation. */
	struct mutex		 oo_rec_lock;
	bool			 oo_rec_ready;
	/*
	 * Small records (LAST_ID): the current value, which readers see at
	 * once, and whether it still has to be logged. See osd_rec_small().
	 */
#define OSD_REC_SMALL_MAX	16
	bool			 oo_rec_cached;
	bool			 oo_rec_dirty;
	u8			 oo_rec_len;
	u8			 oo_rec_val[OSD_REC_SMALL_MAX];

	__u32			 oo_lma_flags;
	unsigned int		 oo_destroyed:1;

	/*
	 * Key and record sizes for a fixed-size binary index, latched by
	 * osd_index_try(). Zero for a directory (name) index.
	 */
	__u16			 oo_keysize;
	__u16			 oo_recsize;
	/* key is a NUL-terminated string bounded by oo_keysize */
	unsigned int		 oo_varkey:1;
};

/*
 * Cached state for one OST object sequence, i.e. one O/<seq>/ subtree.
 * Looked up on the FID fast path, so kept small.
 */
struct osd_obj_seq {
	struct list_head	 oos_seq_list;
	u64			 oos_seq;
	struct dentry		*oos_root;	/* O/<seq> */
	struct dentry	       **oos_dirs;	/* O/<seq>/d0..dN-1 */
	int			 oos_subdir_count;
};

struct osd_obj_map {
	struct dentry		*om_root;	/* O/ */
	struct list_head	 om_seq_list;
	struct rw_semaphore	 om_sem;
	int			 om_subdir_count;
};

/*
 * The object index for objects that are NOT OST data objects.
 *
 * OST data objects (IDIF and the OST sequences) are found by deriving their
 * path from the FID, so they need no index at all. Everything else -- the
 * target's local named objects, last_rcvd, reply_data, the llog catalogs, the
 * CONFIGS directory -- lives at an arbitrary place in the namespace and must
 * be reachable by FID.
 *
 * Implementation: a flat directory of symlinks, "oi/<seq>:<oid>:<ver>", whose
 * link body is "<ino>:<gen>". Lookup reads the link and turns the pair into a
 * dentry with the filesystem's own NFS export operation, which validates the
 * generation for us and so detects inode reuse. Rename does not change an
 * inode number, so an OI entry stays valid when an object is bound into the
 * namespace under its name.
 *
 * This is deliberately not a b-tree. An OST has a few hundred such objects,
 * XFS directories are already b-trees, and a symlink body is a cheap way to
 * store 12 bytes without needing a block-mapped index file.
 */
struct osd_oi {
	struct dentry		*oi_root;	/* oi/ */
	struct dentry		*oi_stage;	/* oi/stage/ */
};

/* operation classes, for per-transaction declare accounting */
enum osd_op_type {
	OSD_OT_ATTR_SET = 0,
	OSD_OT_XATTR_SET,
	OSD_OT_XATTR_DEL,
	OSD_OT_CREATE,
	OSD_OT_DESTROY,
	OSD_OT_REF_ADD,
	OSD_OT_REF_DEL,
	OSD_OT_INSERT,
	OSD_OT_DELETE,
	OSD_OT_WRITE,
	OSD_OT_PUNCH,
	OSD_OT_MAX
};

/* what a transaction says it will need, and what it actually took */
struct osd_reservation {
	__u64	or_blocks;
	__u32	or_inodes;
};

/* what one OI scrub pass found */
struct osd_scrub_stats {
	__u64	ss_scanned;	/* objects examined */
	__u64	ss_ok;		/* already had a correct index entry */
	__u64	ss_repaired;	/* entry inserted or corrected */
	__u64	ss_no_lma;	/* no trusted.lma, so not indexable */
	__u64	ss_derivable;	/* OST object, found by path, needs no entry */
	__u64	ss_failed;	/* could not be repaired */
	__u64	ss_too_deep;	/* below OSD_SCRUB_MAX_DEPTH */
	__u64	ss_skipped_dirs;/* O/ and oi/, deliberately not walked */
	__u64	ss_time_ms;
	int	ss_rc;
};

/*
 * Per-target counters published as osd-xfs.<target>.stats.
 *
 * cache_access/hit/miss are read by sanity 151 and 156, which check that the
 * hit counter moves by exactly the number of pages they touched; without them
 * the sum is always zero and the tests abort with "roc_hit is not safe to
 * use". The names have to match osd-ldiskfs, since the tests grep for them.
 */
enum {
	LPROC_OSD_XFS_CACHE_ACCESS	= 0,
	LPROC_OSD_XFS_CACHE_HIT		= 1,
	LPROC_OSD_XFS_CACHE_MISS	= 2,
	LPROC_OSD_XFS_LAST,
};

struct osd_device {
	struct dt_device	 od_dt_dev;

	struct vfsmount		*od_mnt;
	char			 od_mntdev[MAX_OBD_NAME];
	char			 od_svname[MAX_OBD_NAME];

	atomic_t		 od_connects;
	struct lu_site		 od_site;

	/* Publication-ordered callback queue, completed after snapshot barriers. */
	/* Sticky durability errors serialize with successful commit callbacks. */
	struct mutex		 od_durability_lock;
	/* Hook outcome and enrollment precede successful callback visibility. */
	struct mutex		 od_publish_lock;
	int			 od_durability_error;
	struct list_head	 od_commit_list;	/* osd_thandle, seq order */
	spinlock_t		 od_commit_lock;
	struct delayed_work	 od_commit_work;
	struct workqueue_struct	*od_commit_wq;
	u64			 od_commit_seq;	/* last assigned publication */
	u64			 od_commit_seq_done;	/* contiguous durable floor */
	u64			 od_commit_seq_failed;
	int			 od_commit_seq_error;
	atomic_t		 od_commit_pending;
	unsigned int		 od_commit_stopping:1;
	/*
	 * Defer commit callbacks and batch the log forces behind them, the
	 * same shape as jbd2's batched commits. See OSD_COMMIT_DELAY_MS for
	 * what it is worth on SMR. Switchable so that can be retested.
	 */
	unsigned int		 od_commit_defer:1;

	/*
	 * How long the commit worker waits before forcing, milliseconds.
	 * Tunable because the useful value depends on the device: a longer
	 * wait batches more transactions into one log force at the cost of
	 * delaying the commit callbacks, and therefore how quickly a client
	 * learns its writes are committed.
	 */
	unsigned int		 od_commit_delay_ms;

	/*
	 * Quota slave device. Lustre enforces quota itself through this rather
	 * than letting the backing filesystem do it, so the OSD mounts XFS
	 * with accounting only (uqnoenforce et al) and serves the accounting
	 * index from sb->s_qcop. Without a qsd instance nothing ever queries
	 * that index and `lfs quota` reports its figures in brackets as
	 * inaccurate.
	 */
	struct qsd_instance	*od_quota_slave_dt;

	/* Advisory admission accounting, not an XFS allocation reservation. */
	struct mutex		 od_res_lock;
	atomic64_t		 od_res_blocks;
	atomic64_t		 od_res_inodes;
	atomic64_t		 od_res_denied;		/* refused early */
	atomic64_t		 od_res_undeclared;	/* ran without a declare */
	atomic64_t		 od_res_overrun;	/* used more than declared */
	atomic64_t		 od_dirty_flushed;	/* buffered writes flushed */
	atomic64_t		 od_dirty_txn;		/* txns needing a flush */
	atomic64_t		 od_dirty_overflow;	/* tracking array too small */
	unsigned int		 od_flush_buffered:1;
	unsigned int		 od_reserve_check:1;	/* consult free space */
	unsigned int		 od_nonrotational:1;	/* data on flash */
	/* the XFS realtime device holds the data; metadata stays on the SSD */
	unsigned int		 od_external_rt:1;
	/*
	 * How fallocate() behaves, matching osd-ldiskfs:
	 *   -1  disabled, return -EOPNOTSUPP
	 *    0  allocate unwritten extents
	 *    1  zero the allocated extents on disk
	 * Exposed as osd-xfs.<target>.fallocate_zero_blocks, which is where
	 * check_set_fallocate() looks before running the fallocate tests.
	 */
	int			 od_fallocate_zero_blocks;
	struct lprocfs_stats	*od_stats;

	atomic64_t		 od_statfs_calls;
	unsigned int		 od_declare_check:1;	/* warn on mismatch */

	/* OI scrub state; see osd_scrub.c */
	struct mutex		 od_scrub_mutex;
	struct task_struct	*od_scrub_task;
	struct osd_scrub_stats	 od_scrub_last;
	unsigned int		 od_scrub_running:1;
	unsigned int		 od_scrub_stop:1;
	/*
	 * procfs root for this target. Only needed because qsd_init() creates
	 * its parameters under a proc_dir_entry; everything osd-xfs registers
	 * itself lives in debugfs via dt_tunables_init().
	 */
	struct proc_dir_entry	*od_proc_entry;
	/* how many transactions each log force covered, for lprocfs */
	atomic64_t		 od_commit_forces;
	atomic64_t		 od_commit_batched;
	/* FID -> object mapping: the O/ directory tree, for OST data objects */
	struct osd_obj_map	*od_ost_map;
	/* FID -> object mapping for everything else */
	struct osd_oi		*od_oi;

	/* root of the backing filesystem */
	struct dentry		*od_root_dentry;
	struct lu_fid		 od_root_fid;

	unsigned int		 od_read_cache:1,
				 od_writethrough_cache:1;
};

/*
 * OSD transaction state. A dt transaction is a sequence of XFS transactions,
 * ordered; see the comment at the top of this file.
 */
struct osd_thandle {
	struct thandle		 ot_super;
	/* OSD publication order, not an XFS CIL sequence. */
	u64			 ot_commit_seq;
	/* linkage on osd_device.od_commit_list */
	struct list_head	 ot_commit_link;

	/*
	 * Set when trans_stop's caller wants this transaction durable before
	 * it returns, rather than letting the worker complete it later. The
	 * worker does the same work either way; with this set it signals the
	 * completion instead of freeing the handle, and the waiter takes
	 * ot_commit_rc and frees. That lets a th_sync transaction share one
	 * writeback and one log force with everything else in the batch
	 * instead of doing its own.
	 */
	struct completion	*ot_commit_done;
	int			 ot_commit_rc;
	int			 ot_res_error;

	/*
	 * Small records (LAST_ID) this transaction wrote. They are logged at
	 * trans_stop, after every effect of the transaction. Precreate writes
	 * LAST_ID before its creates; logging it then would let a crash keep
	 * a LAST_ID covering objects that were never created, which nothing
	 * repairs.
	 */
#define OSD_TH_MAX_DEFER	4
	struct osd_object	*ot_defer[OSD_TH_MAX_DEFER];
	int			 ot_ndefer;

	/*
	 * Inodes this transaction wrote with buffered I/O, so trans_stop can
	 * get that data onto stable storage. Almost always one (last_rcvd);
	 * the array avoids an allocation for the common case and anything
	 * beyond it falls back to flushing the whole filesystem.
	 */
#define OSD_TH_MAX_DIRTY	16
	struct inode		*ot_dirty[OSD_TH_MAX_DIRTY];
	int			 ot_ndirty;
	unsigned int		 ot_dirty_overflow:1;

	/* what this transaction declared, and what it has consumed */
	struct osd_reservation	 ot_res;
	struct osd_reservation	 ot_used;
	int			 ot_declare_ops[OSD_OT_MAX];
	int			 ot_exec_ops[OSD_OT_MAX];
	unsigned int		 ot_res_claimed:1;

	/*
	 * Quota ids this transaction has declared against, so qsd_op_end() can
	 * release the reservations when it finishes. Points at the per-thread
	 * oti_quota_trans; NULL once handed to trans_stop.
	 */
	struct lquota_trans	*ot_quota_trans;
	/* ids already declared, to avoid declaring the same one twice */
	struct osd_declared_qid {
		union lquota_id	 oq_id;
		short		 oq_type;
		bool		 oq_blk;
	}			 ot_id_array[OSD_MAX_UGID_CNT];
	int			 ot_id_cnt;
	struct list_head	 ot_commit_dcb_list;
	struct list_head	 ot_stop_dcb_list;
	unsigned int		 ot_started:1;
};

struct osd_thread_info {
	struct ost_id		 oti_ostid;

	/* scratch space for building O/<seq>/dN/<oid> names */
	char			 oti_name[64];

	struct lustre_mdt_attrs	 oti_mdt_attrs;

	/* quota: per-thread scratch for a declare, and the ids of one txn */
	struct lquota_id_info	 oti_qi;
	struct lquota_trans	 oti_quota_trans;
};

extern struct lu_context_key osd_key;

static inline struct osd_thread_info *osd_oti_get(const struct lu_env *env)
{
	return lu_context_key_get(&env->le_ctx, &osd_key);
}

/* ------------------------------------------------------------------ */
/* container_of helpers                                               */
/* ------------------------------------------------------------------ */

static inline struct osd_device *osd_dt_dev(const struct dt_device *d)
{
	return container_of_safe(d, struct osd_device, od_dt_dev);
}

static inline struct osd_device *osd_dev(const struct lu_device *d)
{
	return osd_dt_dev(container_of_safe(d, struct dt_device, dd_lu_dev));
}

static inline struct osd_object *osd_obj(const struct lu_object *o)
{
	return container_of_safe(o, struct osd_object, oo_dt.do_lu);
}

static inline struct osd_object *osd_dt_obj(const struct dt_object *d)
{
	return osd_obj(&d->do_lu);
}

static inline struct osd_device *osd_obj2dev(const struct osd_object *o)
{
	return osd_dev(o->oo_dt.do_lu.lo_dev);
}

static inline struct super_block *osd_sb(const struct osd_device *dev)
{
	return dev->od_mnt ? dev->od_mnt->mnt_sb : NULL;
}

static inline struct lu_device *osd2lu_dev(struct osd_device *osd)
{
	return &osd->od_dt_dev.dd_lu_dev;
}

static inline char *osd_name(struct osd_device *osd)
{
	return osd->od_dt_dev.dd_lu_dev.ld_obd->obd_name;
}

static inline struct osd_thandle *osd_thandle(struct thandle *th)
{
	return container_of_safe(th, struct osd_thandle, ot_super);
}

/* ------------------------------------------------------------------ */
/* osd_oi.c - the O/<seq>/dN/<oid> object map                          */
/* ------------------------------------------------------------------ */

int  osd_obj_map_init(const struct lu_env *env, struct osd_device *dev);
void osd_obj_map_fini(struct osd_device *dev);
int  osd_obj_map_lookup(const struct lu_env *env, struct osd_device *dev,
			const struct lu_fid *fid, struct dentry **dentry);
int  osd_obj_map_create(const struct lu_env *env, struct osd_device *dev,
			const struct lu_fid *fid, umode_t mode,
			const struct xfs_osd_attr *attr,
			const struct xfs_osd_xattr *xattrs, unsigned int nr,
			struct dentry **dentry);
int  osd_obj_map_delete(const struct lu_env *env, struct osd_device *dev,
			const struct lu_fid *fid);

/*
 * Batched directory reader.
 *
 * A readdir actor that returns false is telling the filesystem "I did not take
 * that entry", so the filesystem leaves its position on it. Emitting one entry
 * per iterate_dir() call therefore re-reads the same entry forever -- which
 * hung the mount inside llog_process_thread with the CPU pinned. The actor
 * must consume entries and return true, and stop only when the batch is full.
 */
#define OSD_DIR_BATCH	32

struct osd_dir_ent {
	u64		de_ino;
	u64		de_off;		/* this entry's own offset */
	int		de_namelen;
	unsigned int	de_type;
	char		de_name[NAME_MAX + 1];
};

struct osd_dir_reader {
	struct file		*dr_file;
	struct osd_dir_ent	*dr_ents;
	int			 dr_nents;
	int			 dr_cur;
};

int  osd_dir_reader_init(struct osd_dir_reader *r, struct osd_device *osd,
			 struct dentry *dentry);
void osd_dir_reader_fini(struct osd_dir_reader *r);
/* 0 = *ent valid, 1 = end of directory, <0 = error */
int  osd_dir_reader_next(struct osd_dir_reader *r, struct osd_dir_ent **ent);
void osd_dir_reader_seek(struct osd_dir_reader *r, loff_t pos);

/* read a symlink body into a kernel buffer (NOT vfs_readlink: that is
 * the readlink(2) backend and copies to userspace) */
int  osd_readlink_str(struct dentry *link, char *buf, size_t len);


/* empty a directory so it can be rmdir'd (an index being destroyed) */
int  osd_purge_dir(struct osd_device *osd, struct dentry *dir, int depth);
#define osd_purge_index_dir(osd, d)	osd_purge_dir((osd), (d), 0)

/* true when the FID's location can be derived from the FID itself */
bool osd_fid_is_on_ost(const struct lu_fid *fid);

/* the oi/ symlink index, for everything whose path cannot be derived */
int  osd_oi_init(const struct lu_env *env, struct osd_device *dev);
void osd_oi_fini(struct osd_device *dev);
struct dentry *osd_lookup_or_mkdir(struct osd_device *dev,
				   struct dentry *parent, const char *name,
				   bool create);

/* declare phase and space reservation; see osd_reserve.c */
void osd_declare_op(struct thandle *th, enum osd_op_type op, __u64 blocks,
		    __u32 inodes);
void osd_exec_op(const struct lu_env *env, struct thandle *th,
		 enum osd_op_type op, __u64 blocks, __u32 inodes);
int  osd_reserve_claim(const struct lu_env *env, struct osd_device *osd,
		       struct osd_thandle *oh);
void osd_reserve_release(struct osd_device *osd, struct osd_thandle *oh);
void osd_reserve_init(struct osd_device *osd);
int osd_commit_error(struct osd_device *osd, int error);
int osd_force_log(struct osd_device *osd);
bool osd_rec_logged(const struct osd_object *obj);
bool osd_rec_small(const struct osd_object *obj);
int osd_rec_small_load(const struct lu_env *env, struct osd_object *obj);
int osd_rec_resize(struct osd_object *obj, u64 size);
int osd_rec_recover(struct osd_object *obj);
int osd_rec_defer_flush(const struct lu_env *env, struct osd_thandle *oh);
void osd_trans_note_dirty(struct thandle *th, struct inode *inode);
void osd_trans_drop_dirty(struct osd_thandle *oh);
bool osd_trans_writeback_dirty(const struct lu_env *env,
			       struct osd_device *osd,
			       struct osd_thandle *oh, int *rcp);
bool osd_commit_writeback_batch(struct osd_device *osd,
				struct list_head *batch, int *rcp);
const char *osd_op_name(enum osd_op_type op);

/* osd_scrub.c */
int  osd_scrub_run(const struct lu_env *env, struct osd_device *dev);
int  osd_scrub_start(struct osd_device *dev);
void osd_scrub_stop(struct osd_device *dev);

int  osd_oi_lookup(const struct lu_env *env, struct osd_device *dev,
		   const struct lu_fid *fid, struct dentry **dentry);
int  osd_oi_insert(const struct lu_env *env, struct osd_device *dev,
		   const struct lu_fid *fid, struct inode *inode);
int  osd_oi_delete(const struct lu_env *env, struct osd_device *dev,
		   const struct lu_fid *fid);

/* unified entry point: dispatches on osd_fid_is_on_ost() */
int  osd_fid_lookup(const struct lu_env *env, struct osd_device *dev,
		    const struct lu_fid *fid, struct dentry **dentry);

/* create an object in oi/stage/, to be bound into the namespace by dio_insert */
int  osd_stage_create(const struct lu_env *env, struct osd_device *dev,
		      const struct lu_fid *fid, umode_t mode,
		      const struct xfs_osd_attr *attr,
		      const struct xfs_osd_xattr *xattrs, unsigned int nr,
		      struct dentry **dentry);
/* move a staged object to <parent>/<name> */
int  osd_stage_bind(const struct lu_env *env, struct osd_device *dev,
		    struct dentry *parent, const char *name,
		    struct dentry *staged);

/* ------------------------------------------------------------------ */
/* osd_object.c                                                        */
/* ------------------------------------------------------------------ */

extern const struct dt_object_operations osd_obj_ops;
extern const struct dt_body_operations   osd_body_ops;
extern const struct dt_body_operations   osd_body_ops_new;
extern const struct dt_index_operations  osd_dir_ops;
extern const struct dt_index_operations  osd_kv_ops;
extern const struct dt_index_operations  osd_otable_ops;
extern const struct dt_index_operations  osd_acct_index_ops;

/* quota: accounting via sb->s_qcop, project id via i_op->fileattr_* */
bool osd_acct_available(struct osd_device *osd, const struct lu_fid *fid);
int  osd_declare_inode_qid(const struct lu_env *env, qid_t uid, qid_t gid,
			   __u32 projid, long long space,
			   struct osd_thandle *oh, struct osd_object *obj,
			   enum osd_quota_local_flags *local_flags, bool blk);
__u32 osd_get_projid(struct inode *inode);
int  osd_set_projid(struct inode *inode, __u32 projid);
extern const struct lu_object_operations osd_lu_obj_ops;
extern const struct lu_device_operations osd_lu_ops;

struct kmem_cache *osd_xfs_object_cachep(void);

struct lu_object *osd_object_alloc(const struct lu_env *env,
				   const struct lu_object_header *hdr,
				   struct lu_device *d);

int osd_lma_get(const struct lu_env *env, struct inode *inode,
		struct lu_fid *fid);
int osd_lma_get_flags(const struct lu_env *env, struct inode *inode,
		      struct lu_fid *fid, __u32 *lma_flags);
int osd_lma_set_flags(const struct lu_env *env, struct inode *inode,
		      __u32 lma_flags);
void osd_lma_build(const struct lu_env *env, const struct lu_fid *fid,
		   struct xfs_osd_xattr *xattr);

/* ------------------------------------------------------------------ */
/* osd_lproc.c                                                         */
/* ------------------------------------------------------------------ */

int  osd_procfs_init(struct osd_device *osd, const char *name);
void osd_procfs_fini(struct osd_device *osd);
int osd_process_param(struct osd_device *osd, struct lustre_cfg *cfg);

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

/*
 * Lustre owns the externally visible timestamps. Mark backing inodes
 * S_NOCMTIME so incidental VFS activity does not move them.
 *
 * This only suppresses the VFS-level updates. xfs_trans_ichgtime() is called
 * unconditionally from XFS's own directory and attribute helpers, so setting
 * an xattr still moves ctime.
 */
static inline void osd_set_nocmtime(struct inode *inode)
{
	if (inode)
		inode->i_flags |= S_NOCMTIME;
}

#endif /* _OSD_XFS_INTERNAL_H */
