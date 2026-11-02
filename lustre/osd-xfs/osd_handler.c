// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Top half of the XFS OSD: module registration, device lifecycle, mounting
 * the backing XFS filesystem, and struct dt_device_operations.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/statfs.h>

#include <lustre_compat.h>
#include <lustre_lfsck.h>
#include <obd_class.h>
#include <uapi/linux/lustre/lustre_param.h>

#include "osd_internal.h"

static struct kmem_cache *osd_object_kmem;

/* ------------------------------------------------------------------ */
/* thread info key                                                     */
/* ------------------------------------------------------------------ */

static void *osd_key_init(const struct lu_context *ctx,
			  struct lu_context_key *key)
{
	struct osd_thread_info *info;

	OBD_ALLOC_PTR(info);
	if (info == NULL)
		return ERR_PTR(-ENOMEM);
	return info;
}

static void osd_key_fini(const struct lu_context *ctx,
			 struct lu_context_key *key, void *data)
{
	struct osd_thread_info *info = data;

	OBD_FREE_PTR(info);
}

static void osd_key_exit(const struct lu_context *ctx,
			 struct lu_context_key *key, void *data)
{
}

struct lu_context_key osd_key = {
	/* lu_context_key_register() asserts on a NULL owner */
	.lct_owner = THIS_MODULE,
	.lct_tags = LCT_DT_THREAD | LCT_MD_THREAD | LCT_MG_THREAD | LCT_LOCAL,
	.lct_init = osd_key_init,
	.lct_fini = osd_key_fini,
	.lct_exit = osd_key_exit
};

/* ------------------------------------------------------------------ */
/* mount / umount of the backing XFS filesystem                        */
/* ------------------------------------------------------------------ */

static int osd_mount(const struct lu_env *env, struct osd_device *o,
		     struct lustre_cfg *cfg)
{
	const char *name = lustre_cfg_string(cfg, 0);
	const char *dev = lustre_cfg_string(cfg, 1);
	const char *opts;
	unsigned long s_flags = 0, lmd_flags = 0;
	struct file_system_type *type;
	char *options = NULL;
	const char *str;
	int rc = 0;

	ENTRY;

	if (o->od_mnt != NULL)
		RETURN(0);

	if (strlen(dev) >= sizeof(o->od_mntdev))
		RETURN(-E2BIG);
	strcpy(o->od_mntdev, dev);

	str = lustre_cfg_buf(cfg, 2);
	sscanf(str, "%lu:%lu", &s_flags, &lmd_flags);

	/* Namespace initialization and recovery currently require writes. */
	if (test_bit(LMD_FLG_DEV_RDONLY, &lmd_flags) ||
	    (s_flags & SB_RDONLY)) {
		CERROR("%s: read-only backing mounts are not supported by "
		       OSD_XFS_NAME " initialization\n", name);
		RETURN(-EOPNOTSUPP);
	}

	opts = lustre_cfg_string(cfg, 3);

	OBD_ALLOC_GFP(options, PAGE_SIZE, GFP_KERNEL);
	if (options == NULL)
		RETURN(-ENOMEM);
	*options = '\0';

	/*
	 * attr2 keeps the attribute fork packed, which is what keeps
	 * trusted.lma and trusted.fid in shortform. inode64 is required for
	 * a target of any size. We do not pass discard: Lustre issues its
	 * own.
	 */
	strncat(options, "attr2,inode64", PAGE_SIZE - 1);

	/*
	 * Pass backend options, including logbsize and logbufs, through to XFS.
	 * Keep XFS defaults: logbsize is buffer capacity, not a minimum write
	 * length. Forced partial records write only their payload and headers,
	 * rounded to log-sector/stripe alignment. Increasing capacity does not
	 * combine forces and can increase the header and padding overhead.
	 * These options require a fresh backing mount, not a remount.
	 */
	if (opts != NULL && opts[0] != '\0') {
		strncat(options, ",", PAGE_SIZE - strlen(options) - 1);
		strncat(options, opts, PAGE_SIZE - strlen(options) - 1);
	}

	type = get_fs_type(OSD_XFS_FSTYPE);
	if (type == NULL) {
		CERROR("%s: cannot find the %s module\n", name,
		       OSD_XFS_FSTYPE);
		GOTO(out, rc = -ENODEV);
	}

	s_flags |= SB_KERNMOUNT;
	o->od_mnt = vfs_kern_mount(type, s_flags, dev, options);
	module_put(type->owner);

	if (IS_ERR(o->od_mnt)) {
		rc = PTR_ERR(o->od_mnt);
		o->od_mnt = NULL;
		CERROR("%s: cannot mount %s as %s: rc = %d\n", name, dev,
		       OSD_XFS_FSTYPE, rc);
		GOTO(out, rc);
	}
	if (sb_rdonly(osd_sb(o))) {
		CERROR("%s: cannot initialize a read-only XFS backing mount\n",
		       name);
		mntput(o->od_mnt);
		o->od_mnt = NULL;
		GOTO(out, rc = -EROFS);
	}

	o->od_root_dentry = osd_sb(o)->s_root;

	LCONSOLE_INFO("%s: mounted %s as an " OSD_XFS_NAME " target\n",
		      name, dev);
	GOTO(out, rc = 0);

out:
	if (options != NULL)
		OBD_FREE(options, PAGE_SIZE);
	return rc;
}

static void osd_umount(const struct lu_env *env, struct osd_device *o)
{
	ENTRY;

	if (o->od_mnt != NULL) {
		shrink_dcache_sb(osd_sb(o));
		mntput(o->od_mnt);
		o->od_mnt = NULL;
	}

	EXIT;
}

/* ------------------------------------------------------------------ */
/* dt_device_operations                                                */
/* ------------------------------------------------------------------ */

static int osd_root_get(const struct lu_env *env, struct dt_device *dev,
			struct lu_fid *f)
{
	struct osd_device *osd = osd_dt_dev(dev);

	*f = osd->od_root_fid;
	return 0;
}

static int osd_statfs(const struct lu_env *env, struct dt_device *d,
		      struct obd_statfs *osfs, struct obd_statfs_info *info)
{
	struct osd_device *osd = osd_dt_dev(d);
	struct super_block *sb = osd_sb(osd);
	struct kstatfs ksfs;
	int rc;

	ENTRY;

	if (unlikely(sb == NULL))
		RETURN(-EINPROGRESS);

	rc = sb->s_op->statfs(sb->s_root, &ksfs);
	if (rc)
		RETURN(rc);

	statfs_pack(osfs, &ksfs);
	osfs->os_type = LL_SUPER_MAGIC;

	/*
	 * os_state has to be set explicitly, not left at whatever
	 * statfs_pack() happened to leave. The client turns these bits into
	 * the per-OST status letters that `lfs df -v` reports, and sanity 56c
	 * compares them against osc.*.statfs_state: an unset os_state read
	 * back as "DN", degraded plus nocreate, which claims the target is
	 * both rebuilding and refusing objects.
	 */
	osfs->os_state = 0;
	if (unlikely(sb->s_flags & SB_RDONLY))
		osfs->os_state |= OS_STATFS_READONLY;

	/*
	 * The largest single object this target can hold. Without it the
	 * client sees 0 and substitutes its own ceiling of 2^63-1, so
	 * sanity 44d truncates to that offset and the write fails at a size
	 * XFS was never going to accept. osd-ldiskfs reports the same figure
	 * here that it puts in ddp_maxbytes, so do that.
	 */
	osfs->os_maxbytes = sb->s_maxbytes;

	/*
	 * Lustre reserves a slice of the target so that a full OST can still
	 * complete the metadata updates needed to delete objects. ldiskfs
	 * gets this from the ext4 reserved block count; XFS has no equivalent
	 * notion exposed here, so apply the same 0.1% (min 32MiB) policy
	 * explicitly rather than letting the target run to genuine zero.
	 */
	if (osfs->os_blocks > 0) {
		__u64 reserved = osfs->os_blocks >> 10;
		__u64 min_resv = (32 << 20) / osfs->os_bsize;

		if (reserved < min_resv)
			reserved = min_resv;
		if (osfs->os_bfree > reserved)
			osfs->os_bfree -= reserved;
		else
			osfs->os_bfree = 0;
		if (osfs->os_bavail > reserved)
			osfs->os_bavail -= reserved;
		else
			osfs->os_bavail = 0;
	}

	osfs->os_namelen = min_t(__u32, osfs->os_namelen, NAME_MAX);

	RETURN(0);
}

static void osd_conf_get(const struct lu_env *env,
			 const struct dt_device *dev,
			 struct dt_device_param *param)
{
	struct osd_device *osd = osd_dt_dev(dev);
	struct super_block *sb = osd_sb(osd);
	int ea_overhead;

	param->ddp_max_name_len = NAME_MAX;
	param->ddp_max_nlink = XFS_MAXLINK;
	param->ddp_symlink_max = sb ? sb->s_blocksize : PAGE_SIZE;
	param->ddp_mount_type = LDD_MT_XFS;

	param->ddp_max_ea_size = XFS_XATTR_SIZE_MAX;
	/*
	 * XFS reports s_maxbytes as 8 EiB (2^63-1) on 64-bit, the format
	 * limit, and Lustre hands that to clients as ocd_maxbytes.
	 *
	 * It has to be capped. With a 2-stripe layout sanity 44d truncates to
	 * max_object_bytes rounded down to the stripe size and writes there;
	 * at 8 EiB that offset is within a byte or two of MAX_LFS_FILESIZE,
	 * so the truncate fails with EFBIG and dd cannot even seek. sanity
	 * 44c meanwhile writes one byte at max_object_bytes - 1 and needs the
	 * object to reach exactly that size, so the cap cannot be arbitrary
	 * either: it must be a size the backing filesystem really accepts.
	 *
	 * 16 TiB satisfies both, and is the figure osd-ldiskfs reports from
	 * the ext4 extent limit, so the two backends advertise alike.
	 */
	param->ddp_maxbytes = min_t(loff_t, sb ? sb->s_maxbytes :
					 MAX_LFS_FILESIZE,
				    OSD_XFS_MAX_OBJECT_BYTES(sb ?
						sb->s_blocksize : 4096));
	param->ddp_mntopts = MNTOPT_USERXATTR | MNTOPT_ACL;

	param->ddp_brw_size = DT_DEF_BRW_SIZE;

	/*
	 * Largest extent to advertise. XFS itself can map up to MAXEXTLEN
	 * (2^21 - 1 blocks), but this value is not only a mapping hint: the
	 * OFD's grant accounting sizes a client's grant from it, so a large
	 * figure hands out a proportionally large grant.
	 *
	 * At 1 << 20 the OST granted 10,240,000 KiB against osd-ldiskfs's
	 * 279,104 KiB on the same free space, 37x, and sanity 64d caught it
	 * as cur_grant running past want_grant + grant_chunk. Use the same
	 * 16384 blocks osd-ldiskfs derives from EXT_INIT_MAX_LEN >> 1, which
	 * is well inside what XFS can map and makes the two backends grant
	 * alike.
	 */
	param->ddp_max_extent_blks = 16384;

	/*
	 * Per-object metadata overhead that grant accounting must reserve.
	 * An XFS inode plus a minimal extent-tree footprint.
	 */
	ea_overhead = 1024;
	param->ddp_inodespace = ea_overhead;

	/*
	 * Worst-case extra blocks charged per extent. XFS bmbt growth is
	 * bounded by XFS_BM_MAXLEVELS; being generous here only costs a
	 * little grant, whereas being stingy risks ENOSPC during writeback.
	 */
	param->ddp_extent_tax = 6 * (sb ? sb->s_blocksize : 4096);

	param->ddp_t10_cksum_type = 0;
	param->ddp_has_lseek_data_hole = true;
}
/*
 * Once durability is uncertain, a later success must not move Lustre's
 * committed watermark past the failed work. Recovery requires unmount,
 * backing-filesystem recovery and Lustre replay in a new device lifetime;
 * there is no live reset because independent VFS mutations cannot be undone.
 * Serialize error publication with callback delivery, not filesystem I/O.
 */
int osd_commit_error(struct osd_device *osd, int error)
{
	mutex_lock(&osd->od_durability_lock);
	if (error != 0 && osd->od_durability_error == 0) {
		osd->od_durability_error = error;
		CERROR("%s: durability failure, fencing commits until unmount/recovery/replay: rc = %d\n",
		       osd_name(osd), error);
	}
	error = osd->od_durability_error;
	mutex_unlock(&osd->od_durability_lock);
	return error;
}


static struct thandle *osd_trans_create(const struct lu_env *env,
					struct dt_device *d)
{
	struct osd_thandle *oh;
	struct thandle *th;

	ENTRY;

	OBD_ALLOC_PTR(oh);
	if (oh == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	th = &oh->ot_super;
	th->th_dev = d;
	th->th_result = 0;
	INIT_LIST_HEAD(&oh->ot_commit_dcb_list);
	INIT_LIST_HEAD(&oh->ot_stop_dcb_list);

	RETURN(th);
}

static int osd_trans_start(const struct lu_env *env, struct dt_device *d,
			   struct thandle *th)
{
	struct osd_thandle *oh = osd_thandle(th);
	struct osd_device *osd = osd_dt_dev(d);
	int rc;

	ENTRY;
	rc = osd_commit_error(osd, 0);
	if (rc != 0) {
		oh->ot_res_error = rc;
		RETURN(rc);
	}

	/*
	 * The target start hook declares recovery-record and version updates
	 * before advisory admission. The stop hook assigns the recovery transno
	 * and writes last_rcvd only after execution, under od_publish_lock.
	 */
	rc = dt_txn_hook_start(env, d, th);
	if (rc != 0) {
		oh->ot_res_error = rc;
		RETURN(rc);
	}

	/* Advisory admission can refuse execution, but cannot reserve XFS space. */
	rc = osd_reserve_claim(env, osd, oh);
	if (rc == 0)
		oh->ot_started = 1;
	else
		oh->ot_res_error = rc;
	RETURN(rc);
}

/*
 * Force the backing filesystem's log to stable storage.
 *
 * Not sync_filesystem(): that WARNs unless the caller holds sb->s_umount
 * (fs/sync.c:38), which an OSD transaction path cannot hold. ->sync_fs(sb, 1)
 * is what sync_filesystem() ends up calling anyway, and for XFS it is
 * xfs_fs_sync_fs() -> xfs_log_force(mp, XFS_LOG_SYNC), which is exactly the
 * "commit the journal" primitive wanted here.
 */
int osd_force_log(struct osd_device *osd)
{
	struct super_block *sb = osd_sb(osd);
	int rc;

	rc = osd_commit_error(osd, 0);
	if (rc != 0)
		return rc;
	if (sb == NULL)
		rc = -ENODEV;
	else if (sb_rdonly(sb))
		rc = -EROFS;
	else if (sb->s_op == NULL || sb->s_op->sync_fs == NULL)
		rc = -EOPNOTSUPP;
	else
		rc = sb->s_op->sync_fs(sb, 1);
	return osd_commit_error(osd, rc);
}

static void osd_trans_commit_cb_run(struct osd_thandle *oh, int error)
{
	struct dt_txn_commit_cb *dcb, *tmp;

	list_for_each_entry_safe(dcb, tmp, &oh->ot_commit_dcb_list, dcb_linkage) {
		list_del_init(&dcb->dcb_linkage);
		dcb->dcb_func(NULL, &oh->ot_super, dcb, error);
	}
}

static int osd_trans_stop_cb_run(const struct lu_env *env,
				 struct osd_thandle *oh, int result)
{
	struct dt_txn_commit_cb *dcb, *tmp;
	struct osd_device *osd = osd_dt_dev(oh->ot_super.th_dev);

	mutex_lock(&osd->od_durability_lock);
	if (osd->od_durability_error != 0)
		result = osd->od_durability_error;

	list_for_each_entry_safe(dcb, tmp, &oh->ot_stop_dcb_list, dcb_linkage) {
		list_del_init(&dcb->dcb_linkage);
		dcb->dcb_func((struct lu_env *)env, &oh->ot_super, dcb, result);
	}
	mutex_unlock(&osd->od_durability_lock);
	return result;
}
/*
 * Hook outcome and callback delivery share od_publish_lock, so every
 * transaction completes in the order it published, including errors.
 * Always nest od_durability_lock inside od_publish_lock, never the reverse.
 */
static int osd_trans_complete(const struct lu_env *env, struct osd_device *osd,
			      struct osd_thandle *oh, int error)
{
	lockdep_assert_held(&osd->od_publish_lock);
	mutex_lock(&osd->od_durability_lock);
	if (osd->od_durability_error != 0)
		error = osd->od_durability_error;
	osd_trans_commit_cb_run(oh, error);
	mutex_unlock(&osd->od_durability_lock);
	return error;
}

static int osd_trans_stop(const struct lu_env *env, struct dt_device *dt,
			  struct thandle *th)
{
	struct osd_thandle *oh = osd_thandle(th);
	struct osd_device *osd = osd_dt_dev(dt);
	int rc, hook_rc, i;
	bool mutated = oh->ot_ndirty || oh->ot_dirty_overflow;

	ENTRY;

	/* Admission reservations must be released on every exit. */
	if (oh->ot_res_error && !th->th_result)
		th->th_result = oh->ot_res_error;
	for (i = 0; i < OSD_OT_MAX; i++)
		mutated |= oh->ot_exec_ops[i] != 0;

	/*
	 * Log the small records (LAST_ID) after every effect of this
	 * transaction and before its last_rcvd record. XFS recovers a prefix
	 * of committed transactions, so a crash cannot keep the watermark and
	 * lose the objects it covers.
	 */
	rc = osd_rec_defer_flush(env, oh);
	if (rc)
		th->th_result = osd_commit_error(osd, rc);

	mutex_lock(&osd->od_publish_lock);
	/*
	 * No execution means no recovery publication, even after a successful
	 * start. In particular, advisory admission failure must not write
	 * last_rcvd or poison the target. Executed VFS operations may have
	 * partially mutated state: an error there remains conservatively fatal.
	 */
	rc = osd_commit_error(osd, 0);
	if (rc == 0 && th->th_result != 0 && mutated)
		rc = osd_commit_error(osd, th->th_result);
	if (rc != 0)
		th->th_result = rc;

	hook_rc = mutated && oh->ot_started ? dt_txn_hook_stop(env, th) : 0;
	if (hook_rc != 0) {
		CERROR("%s: transaction hook failed: rc = %d\n",
		       osd_name(osd), hook_rc);
		rc = osd_commit_error(osd, hook_rc);
	}
	if (rc == 0)
		rc = th->th_result;
	rc = osd_trans_stop_cb_run(env, oh, rc);

	if (rc == 0 && mutated) {
		int rc2;

		osd_trans_writeback_dirty(env, osd, oh, &rc);
		rc = osd_commit_error(osd, rc);
		rc2 = osd_force_log(osd);
		if (rc == 0)
			rc = rc2;
	}
	osd_trans_drop_dirty(oh);
	osd_reserve_release(osd, oh);
	rc = osd_trans_complete(env, osd, oh, rc);
	mutex_unlock(&osd->od_publish_lock);
	OBD_FREE_PTR(oh);
	RETURN(rc);
}

static int osd_trans_cb_add(struct thandle *th, struct dt_txn_commit_cb *dcb)
{
	struct osd_thandle *oh = osd_thandle(th);

	LASSERT(dcb->dcb_magic == TRANS_COMMIT_CB_MAGIC);
	LASSERT(&dcb->dcb_func != NULL);

	if (dcb->dcb_flags & DCB_TRANS_STOP)
		list_add(&dcb->dcb_linkage, &oh->ot_stop_dcb_list);
	else
		list_add(&dcb->dcb_linkage, &oh->ot_commit_dcb_list);

	return 0;
}

static int osd_sync(const struct lu_env *env, struct dt_device *d)
{
	struct osd_device *osd = osd_dt_dev(d);

	CDEBUG(D_CACHE, "%s: syncing OSD\n", osd_name(osd));
	return osd_force_log(osd);
}

static int osd_commit_async(const struct lu_env *env, struct dt_device *d)
{
	struct osd_device *osd = osd_dt_dev(d);
	struct super_block *sb = osd_sb(osd);

	/* Ask for a non-blocking log push. */
	if (sb != NULL && sb->s_op != NULL && sb->s_op->sync_fs != NULL &&
	    !sb_rdonly(sb))
		return osd_commit_error(osd, sb->s_op->sync_fs(sb, 0));
	return osd_commit_error(osd, 0);
}

/* No device write-drop hook: crash tests power-cut the node instead. */
static int osd_ro(const struct lu_env *env, struct dt_device *d)
{
	return -EOPNOTSUPP;
}

static struct vfsmount *osd_mnt_get(const struct dt_device *dev)
{
	struct osd_device *osd = osd_dt_dev(dev);

	return osd->od_mnt;
}

/* Recover the allocation floor from O/<seq>, not the in-memory map cache. */
static int osd_last_seq_get(const struct lu_env *env, struct dt_device *dt,
			  __u64 *seq)
{
	struct osd_device *osd = osd_dt_dev(dt);
	struct osd_dir_reader reader;
	struct osd_dir_ent *ent;
	struct dentry *root, *child;
	unsigned long long last = 0, found;
	int rc;

	if (!osd->od_ost_map)
		return -ENODEV;
	root = osd->od_ost_map->om_root;
	rc = osd_dir_reader_init(&reader, osd, root);
	if (rc)
		return rc;

	while (!(rc = osd_dir_reader_next(&reader, &ent))) {
		struct qstr name = QSTR_INIT(ent->de_name, ent->de_namelen);
		bool is_dir;

		/* Do not rely on d_type: the ldiskfs scan checks the inode. */
		inode_lock_shared(d_inode(root));
		child = lookup_noperm(&name, root);
		inode_unlock_shared(d_inode(root));
		if (IS_ERR(child)) {
			rc = PTR_ERR(child);
			break;
		}
		if (d_really_is_negative(child)) {
			dput(child);
			rc = -ENOENT;
			break;
		}
		is_dir = S_ISDIR(d_inode(child)->i_mode);
		dput(child);
		if (!is_dir)
			continue;

		/*
		 * Normal sequence names are hexadecimal. Reserved decimal
		 * names and the legacy/IDIF namespace O/0 are all below
		 * FID_SEQ_NORMAL even when read as hex, and cannot constrain
		 * the normal sequence allocator (same rule as ldiskfs).
		 */
		rc = kstrtoull(ent->de_name, 16, &found);
		if (rc)
			break;
		if (found == U64_MAX) {
			rc = -EOVERFLOW;
			break;
		}
		if (found >= FID_SEQ_NORMAL && found > last)
			last = found;
	}
	osd_dir_reader_fini(&reader);
	if (rc < 0)
		return rc;
	*seq = last;
	return 0;
}

static const struct dt_device_operations osd_dt_ops = {
	.dt_root_get		  = osd_root_get,
	.dt_statfs		  = osd_statfs,
	.dt_trans_create	  = osd_trans_create,
	.dt_trans_start		  = osd_trans_start,
	.dt_trans_stop		  = osd_trans_stop,
	.dt_trans_cb_add	  = osd_trans_cb_add,
	.dt_conf_get		  = osd_conf_get,
	.dt_mnt_get		  = osd_mnt_get,
	.dt_sync		  = osd_sync,
	.dt_ro			  = osd_ro,
	.dt_commit_async	  = osd_commit_async,
	.dt_last_seq_get		  = osd_last_seq_get,
};

/* ------------------------------------------------------------------ */
/* lu_device lifecycle                                                 */
/* ------------------------------------------------------------------ */

static int osd_shutdown(const struct lu_env *env, struct osd_device *o)
{
	ENTRY;


	RETURN(0);
}

static int osd_device_init0(const struct lu_env *env, struct osd_device *o,
			    struct lustre_cfg *cfg)
{
	struct lu_device *l = osd2lu_dev(o);

	const char *name;
	int rc;
	const char *svname;
	ENTRY;

	/* if the device is already initialised, do nothing */
	if (o->od_mnt != NULL)
		RETURN(0);

	name = lustre_cfg_string(cfg, 0);
	if (name == NULL)
		RETURN(-EINVAL);


	l->ld_ops = &osd_lu_ops;
	o->od_dt_dev.dd_ops = &osd_dt_ops;
	atomic_set(&o->od_connects, 0);

	/*
	 * The *service* name (lustre-OST0000) comes from config argument 4,
	 * not from argument 0, which is the device name and carries an "-osd"
	 * suffix.
	 */
	svname = lustre_cfg_string(cfg, 4);
	if (svname == NULL || svname[0] == '\0')
		svname = name;
	strscpy(o->od_svname, svname, sizeof(o->od_svname));

	if (strstr(o->od_svname, "-OST") == NULL) {
		CERROR("%s: " OSD_XFS_NAME " currently supports OST targets "
		       "only; the MGT and MDT must stay on ldiskfs or ZFS\n",
		       name);
		RETURN(-EOPNOTSUPP);
	}

	rc = osd_mount(env, o, cfg);
	if (rc != 0)
		RETURN(rc);

	lu_local_obj_fid(&o->od_root_fid, OSD_FS_ROOT_OID);

	rc = lu_site_init(&o->od_site, l);
	if (rc != 0)
		GOTO(out_mnt, rc);
	o->od_site.ls_bottom_dev = l;

	rc = lu_site_init_finish(&o->od_site);
	if (rc != 0)
		GOTO(out_site, rc);

	rc = osd_obj_map_init(env, o);
	if (rc != 0)
		GOTO(out_site, rc);

	rc = osd_oi_init(env, o);
	if (rc != 0)
		GOTO(out_map, rc);

	osd_reserve_init(o);

	/*
	 * Register under od_svname, not name. `name` is the OSD device name
	 * and carries the "-osd" suffix, so the tunables appeared as
	 * osd-xfs.lustre-OST0000-osd.* where osd-ldiskfs publishes
	 * osd-ldiskfs.lustre-OST0001.*. Anything addressing an OSD by its
	 * service name then missed.
	 */
	rc = osd_procfs_init(o, o->od_svname);
	if (rc != 0)
		CWARN("%s: tunables unavailable: rc = %d\n", name, rc);

	RETURN(0);

out_map:
	osd_obj_map_fini(o);
out_site:
	lu_site_fini(&o->od_site);
out_mnt:
	osd_umount(env, o);
	RETURN(rc);
}

static struct lu_device *osd_device_alloc(const struct lu_env *env,
					  struct lu_device_type *type,
					  struct lustre_cfg *cfg)
{
	struct osd_device *o;
	int rc;

	OBD_ALLOC_PTR(o);
	if (o == NULL)
		return ERR_PTR(-ENOMEM);

	mutex_init(&o->od_publish_lock);
	mutex_init(&o->od_durability_lock);
	o->od_durability_error = 0;

	rc = dt_device_init(&o->od_dt_dev, type);
	if (rc != 0) {
		OBD_FREE_PTR(o);
		return ERR_PTR(rc);
	}

	rc = osd_device_init0(env, o, cfg);
	if (rc != 0) {
		dt_device_fini(&o->od_dt_dev);
		OBD_FREE_PTR(o);
		return ERR_PTR(rc);
	}

	return osd2lu_dev(o);
}

static struct lu_device *osd_device_free(const struct lu_env *env,
					 struct lu_device *d)
{
	struct osd_device *o = osd_dev(d);

	ENTRY;

	/*
	 * Cached objects hold references on the device. Purging the site first
	 * drops them; without this, lu_site_fini() walks into lu_device_put()
	 * with ld_ref already at zero and LBUGs. osd-ldiskfs does the same
	 * dance, including making the OSD its own top device so the purge can
	 * reach everything.
	 */
	d->ld_site->ls_top_dev = d;
	lu_site_purge(env, d->ld_site, -1);
	if (atomic_read(&d->ld_ref) != 0)
		lu_site_print(env, d->ld_site, &d->ld_ref, D_ERROR,
			      lu_cdebug_printer);
	lu_site_fini(&o->od_site);
	dt_device_fini(&o->od_dt_dev);
	OBD_FREE_PTR(o);

	RETURN(NULL);
}

static struct lu_device *osd_device_fini(const struct lu_env *env,
					 struct lu_device *d)
{
	struct osd_device *o = osd_dev(d);

	ENTRY;

	osd_procfs_fini(o);
	osd_oi_fini(o);
	osd_obj_map_fini(o);
	osd_shutdown(env, o);
	osd_umount(env, o);

	RETURN(NULL);
}

static int osd_device_init(const struct lu_env *env, struct lu_device *d,
			   const char *name, struct lu_device *next)
{
	/* everything of substance happened in osd_device_init0() */
	return 0;
}

static int osd_process_config(const struct lu_env *env, struct lu_device *d,
			      struct lustre_cfg *cfg)
{
	struct osd_device *o = osd_dev(d);
	int rc;

	ENTRY;

	switch (cfg->lcfg_command) {
	case LCFG_SETUP:
		rc = osd_mount(env, o, cfg);
		break;
	case LCFG_CLEANUP:
		lu_dev_del_linkage(d->ld_site, d);
		rc = osd_shutdown(env, o);
		break;
	case LCFG_PARAM:
		rc = osd_process_param(o, cfg);
		break;
	default:
		rc = -ENOTTY;
	}

	RETURN(rc);
}


/*
 * Clear the "never registered" marker in CONFIGS/mountdata.
 *
 * Lustre records whether a target has ever registered with the MGS in the
 * separator of ldd_svname: "lustre:OST0000" means virgin, "lustre-OST0000"
 * means registered. mount.lustre turns the colon into a dash in memory, sets
 * LDD_F_VIRGIN, and the MGS then refuses a second registration of an index it
 * already knows ("requested index 0, but that index is already in use").
 *
 * The backends persist the registered form differently:
 *   ldiskfs - ldiskfs_read_ldd() takes ldd_svname from the ext4 *volume label*,
 *             and ldiskfs_label_lustre() rewrites that label after the mount
 *             using tune2fs, which works on a mounted ext4.
 *   ZFS     - zfs_label_lustre() sets the lustre:svname dataset property,
 *             which libzfs can do while the dataset is in use.
 *
 * Neither works for XFS: an XFS label is 12 characters (XFSLABEL_MAX), too
 * short for a service name, and xfs_admin refuses a mounted filesystem. But
 * osd-xfs *owns* the mounted XFS, so it can update CONFIGS/mountdata itself.
 * Doing it here, once the target has finished coming up, is the same point in
 * the lifecycle at which the other two backends record it.
 */
static int osd_clear_virgin_marker(const struct lu_env *env,
				   struct osd_device *osd)
{
	struct lustre_disk_data *ldd = NULL;
	struct dentry *dconf = NULL, *dmd = NULL;
	struct qstr qconf = QSTR_INIT(MOUNT_CONFIGS_DIR,
				      sizeof(MOUNT_CONFIGS_DIR) - 1);
	struct qstr qmd = QSTR_INIT(CONFIGS_FILE, sizeof(CONFIGS_FILE) - 1);
	struct file *file = NULL;
	loff_t pos = 0;
	ssize_t n;
	int len, rc = 0;

	ENTRY;

	CDEBUG(D_MOUNT, "%s: checking the virgin marker\n", osd_name(osd));

	if (osd->od_root_dentry == NULL || osd_sb(osd) == NULL ||
	    sb_rdonly(osd_sb(osd)))
		RETURN(0);

	inode_lock_shared(d_inode(osd->od_root_dentry));
	dconf = lookup_noperm(&qconf, osd->od_root_dentry);
	inode_unlock_shared(d_inode(osd->od_root_dentry));
	if (IS_ERR(dconf))
		RETURN(PTR_ERR(dconf));
	if (d_really_is_negative(dconf))
		GOTO(out, rc = 0);	/* no config dir: nothing to do */

	inode_lock_shared(d_inode(dconf));
	dmd = lookup_noperm(&qmd, dconf);
	inode_unlock_shared(d_inode(dconf));
	if (IS_ERR(dmd))
		GOTO(out, rc = PTR_ERR(dmd));
	if (d_really_is_negative(dmd))
		GOTO(out, rc = 0);

	OBD_ALLOC_PTR(ldd);
	if (ldd == NULL)
		GOTO(out, rc = -ENOMEM);

	file = dentry_open(&(struct path){ .mnt = osd->od_mnt, .dentry = dmd },
			   O_RDWR | O_LARGEFILE, current_cred());
	if (IS_ERR(file))
		GOTO(out, rc = PTR_ERR(file));

	n = kernel_read(file, ldd, sizeof(*ldd), &pos);
	if (n != sizeof(*ldd))
		GOTO(out, rc = (n < 0) ? n : -EIO);

	if (ldd->ldd_magic != LDD_MAGIC)
		GOTO(out, rc = 0);	/* not ours to touch */

	len = strlen(ldd->ldd_svname);
	if (len < 8)
		GOTO(out, rc = 0);

	CDEBUG(D_MOUNT, "%s: mountdata svname '%s' flags %#x\n",
	       osd_name(osd), ldd->ldd_svname, ldd->ldd_flags);

	if (ldd->ldd_svname[len - 8] != ':' &&
	    !(ldd->ldd_flags & LDD_F_VIRGIN))
		GOTO(out, rc = 0);	/* already registered */

	ldd->ldd_svname[len - 8] = '-';
	ldd->ldd_flags &= ~(LDD_F_VIRGIN | LDD_F_UPDATE | LDD_F_NEED_INDEX);

	pos = 0;
	n = kernel_write(file, ldd, sizeof(*ldd), &pos);
	if (n != sizeof(*ldd))
		GOTO(out, rc = (n < 0) ? n : -EIO);

	rc = vfs_fsync(file, 1);
	if (rc == 0)
		LCONSOLE_INFO("%s: recorded first registration in %s (%s)\n",
			      osd_name(osd), MOUNT_DATA_FILE, ldd->ldd_svname);
	EXIT;
out:
	if (!IS_ERR_OR_NULL(file))
		fput(file);
	if (ldd != NULL)
		OBD_FREE_PTR(ldd);
	if (!IS_ERR_OR_NULL(dmd))
		dput(dmd);
	if (!IS_ERR_OR_NULL(dconf))
		dput(dconf);
	if (rc)
		CERROR("%s: cannot clear the virgin marker: rc = %d\n",
		       osd_name(osd), rc);
	return rc;
}

static int osd_recovery_complete(const struct lu_env *env,
				 struct lu_device *d)
{
	struct osd_device *o = osd_dev(d);

	/*
	 * The target is fully up. Record that it has registered, so the next
	 * mount does not ask the MGS for an index it already owns.
	 */
	osd_clear_virgin_marker(env, o);

	return 0;
}

static int osd_prepare(const struct lu_env *env, struct lu_device *pdev,
		       struct lu_device *dev)
{
	struct osd_device *o = osd_dev(dev);

	/*
	 * server_start_targets() registers with the MGS before it processes
	 * the config log that reaches this point, so by now the target either
	 * registered successfully or the mount already failed. Recording it
	 * here as well as in recovery_complete costs nothing and covers stacks
	 * that never call the latter.
	 */
	osd_clear_virgin_marker(env, o);

	return 0;
}

const struct lu_device_operations osd_lu_ops = {
	.ldo_object_alloc	= osd_object_alloc,
	.ldo_process_config	= osd_process_config,
	.ldo_recovery_complete	= osd_recovery_complete,
	.ldo_prepare		= osd_prepare,
	.ldo_fid_alloc		= fid_alloc_generic,
};

static const struct lu_device_type_operations osd_device_type_ops = {
	.ldto_device_alloc	= osd_device_alloc,
	.ldto_device_free	= osd_device_free,
	.ldto_device_init	= osd_device_init,
	.ldto_device_fini	= osd_device_fini,
};

static struct lu_device_type osd_device_type = {
	.ldt_tags	= LU_DEVICE_DT,
	.ldt_name	= LUSTRE_OSD_XFS_NAME,
	.ldt_ops	= &osd_device_type_ops,
	.ldt_ctx_tags	= LCT_LOCAL,
};

/* ------------------------------------------------------------------ */
/* obd_ops                                                             */
/* ------------------------------------------------------------------ */

static int osd_obd_connect(const struct lu_env *env, struct obd_export **exp,
			   struct obd_device *obd, struct obd_uuid *cluuid,
			   struct obd_connect_data *data, void *localdata)
{
	struct osd_device *osd = osd_dev(obd->obd_lu_dev);
	struct lustre_handle conn;
	int rc;

	ENTRY;

	rc = class_connect(&conn, obd, cluuid);
	if (rc)
		RETURN(rc);

	*exp = class_conn2export(&conn);
	atomic_inc(&osd->od_connects);

	RETURN(0);
}

static int osd_obd_disconnect(struct obd_export *exp)
{
	struct obd_device *obd = exp->exp_obd;
	struct osd_device *osd = osd_dev(obd->obd_lu_dev);
	int rc, release;

	ENTRY;

	release = atomic_dec_and_test(&osd->od_connects);
	rc = class_disconnect(exp);

	if (rc == 0 && release)
		class_manual_cleanup(obd);

	RETURN(rc);
}

static int osd_health_check(const struct lu_env *env, struct obd_device *obd)
{
	struct osd_device *osd = osd_dev(obd->obd_lu_dev);
	struct super_block *sb = osd_sb(osd);
	int error;

	error = osd_commit_error(osd, 0);
	if (error != 0)
		CERROR("%s: unhealthy: fence_rc=%d; unmount/recovery/replay required\n",
		       osd_name(osd), error);
	return sb == NULL || sb_rdonly(sb) || error != 0;
}

static const struct obd_ops osd_obd_device_ops = {
	.o_owner	= THIS_MODULE,
	.o_connect	= osd_obd_connect,
	.o_disconnect	= osd_obd_disconnect,
	.o_health_check	= osd_health_check,
};

/* ------------------------------------------------------------------ */
/* module init / exit                                                  */
/* ------------------------------------------------------------------ */

static int __init osd_xfs_init(void)
{
	int rc;

	rc = libcfs_setup();
	if (rc)
		return rc;

	osd_object_kmem = kmem_cache_create("osd_xfs_obj",
					    sizeof(struct osd_object), 0,
					    SLAB_HWCACHE_ALIGN, NULL);
	if (osd_object_kmem == NULL)
		return -ENOMEM;

	rc = lu_context_key_register(&osd_key);
	if (rc)
		GOTO(out_cache, rc);

	rc = class_register_type(&osd_obd_device_ops, NULL, true,
				 LUSTRE_OSD_XFS_NAME, &osd_device_type);
	if (rc)
		GOTO(out_key, rc);

	return 0;

out_key:
	lu_context_key_degister(&osd_key);
out_cache:
	kmem_cache_destroy(osd_object_kmem);
	return rc;
}

static void __exit osd_xfs_exit(void)
{
	class_unregister_type(LUSTRE_OSD_XFS_NAME);
	lu_context_key_degister(&osd_key);
	kmem_cache_destroy(osd_object_kmem);
}

struct kmem_cache *osd_xfs_object_cachep(void)
{
	return osd_object_kmem;
}

MODULE_AUTHOR("OpenSFS <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Object Storage Device (" LUSTRE_OSD_XFS_NAME ")");
MODULE_VERSION(LUSTRE_VERSION_STRING);
MODULE_LICENSE("GPL");

late_initcall_sync(osd_xfs_init);
module_exit(osd_xfs_exit);
