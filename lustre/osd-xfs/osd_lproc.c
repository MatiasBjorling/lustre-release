// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * lprocfs entries for osd-xfs.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <lprocfs_status.h>
#include <obd_class.h>
#include <uapi/linux/lustre/lustre_param.h>

#include "osd_internal.h"

static int osd_mntdev_seq_show(struct seq_file *m, void *data)
{
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);

	seq_printf(m, "%s\n", osd->od_mntdev);
	return 0;
}
LDEBUGFS_SEQ_FOPS_RO(osd_mntdev);

/* Report the backing filesystem's effective options, not requested values. */
static int osd_mount_options_seq_show(struct seq_file *m, void *data)
{
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);
	struct super_block *sb = osd_sb(osd);
	int rc = 0;

	if (sb == NULL || osd->od_mnt == NULL)
		return -ENODEV;
	if (sb->s_op->show_options != NULL)
		rc = sb->s_op->show_options(m, osd->od_mnt->mnt_root);
	seq_putc(m, '\n');
	return rc;
}
LDEBUGFS_SEQ_FOPS_RO(osd_mount_options);

static int osd_fstype_seq_show(struct seq_file *m, void *data)
{
	seq_printf(m, "%s\n", OSD_XFS_FSTYPE);
	return 0;
}
LDEBUGFS_SEQ_FOPS_RO(osd_fstype);

/*
 * Commit accumulator statistics. "batched per force" is the number that says
 * whether deferring commits is doing anything: 1.0 means every transaction is
 * still getting its own log force, which is what this replaced.
 */
static int osd_commit_stats_seq_show(struct seq_file *m, void *data)
{
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);
	u64 forces = atomic64_read(&osd->od_commit_forces);
	u64 batched = atomic64_read(&osd->od_commit_batched);

	seq_printf(m, "log_forces:        %llu\n", forces);
	seq_printf(m, "transactions:      %llu\n", batched);
	seq_printf(m, "batched_per_force: %llu.%02llu\n",
		   forces ? batched / forces : 0,
		   forces ? (batched * 100 / forces) % 100 : 0);
	seq_printf(m, "pending:           %d\n",
		   atomic_read(&osd->od_commit_pending));
	mutex_lock(&osd->od_publish_lock);
	seq_printf(m, "seq_published:     %llu\n", osd->od_commit_seq);
	seq_printf(m, "seq_durable:       %llu\n", osd->od_commit_seq_done);
	seq_printf(m, "seq_failed:        %llu\n", osd->od_commit_seq_failed);
	seq_printf(m, "seq_failed_error:  %d\n", osd->od_commit_seq_error);
	seq_printf(m, "durability_error:  %d\n", osd_commit_error(osd, 0));
	mutex_unlock(&osd->od_publish_lock);
	seq_puts(m, "recovery:          unmount/backing-filesystem recovery/Lustre replay; no live reset\n");
	return 0;
}
LDEBUGFS_SEQ_FOPS_RO(osd_commit_stats);


/* Whether commit callbacks are deferred and their log forces batched. */
static int osd_commit_defer_seq_show(struct seq_file *m, void *data)
{
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);

	seq_printf(m, "%u\n", osd->od_commit_defer);
	return 0;
}

static ssize_t osd_commit_defer_seq_write(struct file *file,
					  const char __user *buffer,
					  size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);
	bool val;
	int rc;

	rc = kstrtobool_from_user(buffer, count, &val);
	if (rc)
		return rc;
	if (val && osd->od_commit_wq == NULL)
		return -EOPNOTSUPP;
	mutex_lock(&osd->od_publish_lock);
	if (atomic_read(&osd->od_commit_pending)) {
		mutex_unlock(&osd->od_publish_lock);
		return -EBUSY;
	}
	osd->od_commit_defer = val;
	mutex_unlock(&osd->od_publish_lock);

	return count;
}
LDEBUGFS_SEQ_FOPS(osd_commit_defer);

/*
 * How long the commit worker waits before forcing the log, milliseconds.
 * Zero forces as soon as the worker is scheduled.
 */
static int osd_commit_delay_ms_seq_show(struct seq_file *m, void *data)
{
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);

	seq_printf(m, "%u\n", osd->od_commit_delay_ms);
	return 0;
}

static ssize_t osd_commit_delay_ms_seq_write(struct file *file,
					     const char __user *buffer,
					     size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);
	unsigned int val;
	int rc;

	rc = kstrtouint_from_user(buffer, count, 0, &val);
	if (rc)
		return rc;
	if (val > 1000)
		return -ERANGE;
	osd->od_commit_delay_ms = val;

	return count;
}
LDEBUGFS_SEQ_FOPS(osd_commit_delay_ms);

/*
 * OI scrub: write 1 to start, 0 to stop; reading reports the last pass.
 *
 * The index maps a FID to an inode for every object whose location is not
 * derivable from its FID. If it is lost those objects are unreachable even
 * though their data is intact, and nothing else on disk holds that mapping.
 */
static int osd_scrub_seq_show(struct seq_file *m, void *data)
{
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);
	struct osd_scrub_stats st;
	bool running;

	mutex_lock(&osd->od_scrub_mutex);
	running = osd->od_scrub_running;
	st = osd->od_scrub_last;
	mutex_unlock(&osd->od_scrub_mutex);

	seq_printf(m, "status:      %s\n", running ? "running" : "idle");
	seq_printf(m, "scanned:     %llu\n", st.ss_scanned);
	seq_printf(m, "correct:     %llu\n", st.ss_ok);
	seq_printf(m, "rebuilt:     %llu\n", st.ss_repaired);
	seq_printf(m, "no_lma:      %llu\n", st.ss_no_lma);
	seq_printf(m, "by_path:     %llu\n", st.ss_derivable);
	seq_printf(m, "failed:      %llu\n", st.ss_failed);
	seq_printf(m, "too_deep:    %llu\n", st.ss_too_deep);
	seq_printf(m, "skipped_dirs:%llu\n", st.ss_skipped_dirs);
	seq_printf(m, "duration_ms: %llu\n", st.ss_time_ms);
	seq_printf(m, "last_rc:     %d\n", st.ss_rc);

	return 0;
}

static ssize_t osd_scrub_seq_write(struct file *file,
				   const char __user *buffer,
				   size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);
	bool val;
	int rc;

	rc = kstrtobool_from_user(buffer, count, &val);
	if (rc)
		return rc;

	if (!val) {
		osd_scrub_stop(osd);
		return count;
	}

	rc = osd_scrub_start(osd);
	if (rc == -EALREADY)
		return count;

	return rc ? rc : count;
}
LDEBUGFS_SEQ_FOPS(osd_scrub);


/*
 * The declare phase and its reservation.
 *
 * "denied" counts transactions refused at trans_start for want of space, which
 * is the whole point: each one is a change that would otherwise have failed
 * partway through. "undeclared" and "overrun" count drift between the declare
 * and execute phases, and both should stay at zero; a non-zero value is a bug
 * in this backend, not a condition to tune around.
 */
static int osd_reserve_seq_show(struct seq_file *m, void *data)
{
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);

	seq_printf(m, "blocks_promised: %lld\n",
		   atomic64_read(&osd->od_res_blocks));
	seq_printf(m, "inodes_promised: %lld\n",
		   atomic64_read(&osd->od_res_inodes));
	seq_printf(m, "denied:          %lld\n",
		   atomic64_read(&osd->od_res_denied));
	seq_printf(m, "undeclared:      %lld\n",
		   atomic64_read(&osd->od_res_undeclared));
	seq_printf(m, "overrun:         %lld\n",
		   atomic64_read(&osd->od_res_overrun));
	seq_printf(m, "declare_check:   %u\n", osd->od_declare_check);
	seq_printf(m, "dirty_txn:       %lld\n",
		   atomic64_read(&osd->od_dirty_txn));
	seq_printf(m, "dirty_flushed:   %lld\n",
		   atomic64_read(&osd->od_dirty_flushed));
	seq_printf(m, "flush_buffered:  %u\n", osd->od_flush_buffered);
	seq_printf(m, "dirty_overflow:  %lld\n",
		   atomic64_read(&osd->od_dirty_overflow));
	seq_printf(m, "reserve_check:   %u\n", osd->od_reserve_check);
	seq_printf(m, "statfs_calls:    %lld\n",
		   atomic64_read(&osd->od_statfs_calls));

	return 0;
}

static ssize_t osd_reserve_seq_write(struct file *file,
				     const char __user *buffer,
				     size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);
	bool val;
	int rc;

	rc = kstrtobool_from_user(buffer, count, &val);
	if (rc)
		return rc;
	osd->od_declare_check = val;

	return count;
}
LDEBUGFS_SEQ_FOPS(osd_reserve);


/*
 * Whether trans_stop flushes the buffered writes of the transaction.
 *
 * This is a correctness switch, not a tuning knob. xfs_fs_sync_fs(sb, 1)
 * forces the log and nothing else, so file data written through osd_write(),
 * which includes the transno in last_rcvd, is not covered by it. With this
 * off, a commit callback can tell Lustre a transno is durable while it is
 * still only in the page cache. It exists so that can be demonstrated.
 */
static int osd_flush_buffered_seq_show(struct seq_file *m, void *data)
{
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);

	seq_printf(m, "%u\n", osd->od_flush_buffered);
	return 0;
}

static ssize_t osd_flush_buffered_seq_write(struct file *file,
					    const char __user *buffer,
					    size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);
	bool val;
	int rc;

	rc = kstrtobool_from_user(buffer, count, &val);
	if (rc)
		return rc;
	osd->od_flush_buffered = val;
	if (!val)
		CWARN("%s: buffered writes will NOT be flushed at trans_stop; a transno can be reported committed while still in memory\n",
		      osd_name(osd));
	return count;
}
LDEBUGFS_SEQ_FOPS(osd_flush_buffered);

/*
 * Whether the declare phase consults free space at all.
 *
 * For attributing cost only. With this off nothing is reserved and a
 * transaction can once again discover -ENOSPC partway through a change it
 * cannot roll back, which is the entire problem the declare phase exists to
 * solve. Not something to leave off.
 */
static int osd_reserve_check_seq_show(struct seq_file *m, void *data)
{
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);

	seq_printf(m, "%u\n", osd->od_reserve_check);
	return 0;
}

static ssize_t osd_reserve_check_seq_write(struct file *file,
					   const char __user *buffer,
					   size_t count, loff_t *off)
{
	struct seq_file *m = file->private_data;
	struct osd_device *osd = osd_dt_dev((struct dt_device *)m->private);
	bool val;
	int rc;

	rc = kstrtobool_from_user(buffer, count, &val);
	if (rc)
		return rc;
	osd->od_reserve_check = val;
	if (!val)
		CWARN("%s: space reservation disabled; -ENOSPC can now strike partway through a transaction\n",
		      osd_name(osd));
	return count;
}
LDEBUGFS_SEQ_FOPS(osd_reserve_check);

/*
 * osd-*.<target>.nonrotational, as a sysfs attribute rather than a debugfs
 * one because that is where callers look for it: sanity 56c writes
 * osd-*.$ost.nonrotational to force the OS_STATFS_NONROT bit on and check
 * the status letters the client derives from it. Writable for the same
 * reason osd-ldiskfs makes it writable: the autodetection reads
 * blk_queue_rot(), which is wrong for a spindle behind a flash cache or a
 * VM's virtio disk, and an operator has to be able to correct it.
 */
static ssize_t nonrotational_show(struct kobject *kobj, struct attribute *attr,
				  char *buf)
{
	struct dt_device *dt = container_of(kobj, struct dt_device, dd_kobj);
	struct osd_device *osd = osd_dt_dev(dt);

	LASSERT(osd);
	if (unlikely(osd->od_mnt == NULL))
		return -EINPROGRESS;

	return sprintf(buf, "%u\n", osd->od_nonrotational);
}

static ssize_t nonrotational_store(struct kobject *kobj,
				   struct attribute *attr, const char *buffer,
				   size_t count)
{
	struct dt_device *dt = container_of(kobj, struct dt_device, dd_kobj);
	struct osd_device *osd = osd_dt_dev(dt);
	bool val;
	int rc;

	LASSERT(osd);
	if (unlikely(osd->od_mnt == NULL))
		return -EINPROGRESS;

	rc = kstrtobool(buffer, &val);
	if (rc)
		return rc;

	osd->od_nonrotational = val;
	return count;
}
LUSTRE_RW_ATTR(nonrotational);

/*
 * osd-*.<target>.read_cache_enable and .writethrough_cache_enable, set from
 * the data device's rotational flag at mount. sanity 151 and 156 read the
 * OST read-cache hit counter and need these to establish their precondition;
 * osd-ldiskfs exposes both as writable, and the tests set them.
 */
static ssize_t read_cache_enable_show(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	struct dt_device *dt = container_of(kobj, struct dt_device, dd_kobj);
	struct osd_device *osd = osd_dt_dev(dt);

	LASSERT(osd);
	if (unlikely(osd->od_mnt == NULL))
		return -EINPROGRESS;

	return sprintf(buf, "%u\n", osd->od_read_cache);
}

static ssize_t read_cache_enable_store(struct kobject *kobj,
				       struct attribute *attr,
				       const char *buffer, size_t count)
{
	struct dt_device *dt = container_of(kobj, struct dt_device, dd_kobj);
	struct osd_device *osd = osd_dt_dev(dt);
	bool val;
	int rc;

	LASSERT(osd);
	if (unlikely(osd->od_mnt == NULL))
		return -EINPROGRESS;

	rc = kstrtobool(buffer, &val);
	if (rc)
		return rc;

	osd->od_read_cache = !!val;
	return count;
}
LUSTRE_RW_ATTR(read_cache_enable);

static ssize_t writethrough_cache_enable_show(struct kobject *kobj,
					      struct attribute *attr,
					      char *buf)
{
	struct dt_device *dt = container_of(kobj, struct dt_device, dd_kobj);
	struct osd_device *osd = osd_dt_dev(dt);

	LASSERT(osd);
	if (unlikely(osd->od_mnt == NULL))
		return -EINPROGRESS;

	return sprintf(buf, "%u\n", osd->od_writethrough_cache);
}

static ssize_t writethrough_cache_enable_store(struct kobject *kobj,
					       struct attribute *attr,
					       const char *buffer, size_t count)
{
	struct dt_device *dt = container_of(kobj, struct dt_device, dd_kobj);
	struct osd_device *osd = osd_dt_dev(dt);
	bool val;
	int rc;

	LASSERT(osd);
	if (unlikely(osd->od_mnt == NULL))
		return -EINPROGRESS;

	rc = kstrtobool(buffer, &val);
	if (rc)
		return rc;

	osd->od_writethrough_cache = !!val;
	return count;
}
LUSTRE_RW_ATTR(writethrough_cache_enable);

/*
 * osd-*.<target>.fallocate_zero_blocks.
 *
 * check_set_fallocate() in the test framework reads this before any
 * fallocate test and skips the whole group when it is absent, which is why
 * sanity 101m, 127f and eleven 150* cases skipped on osd-xfs while passing
 * on ldiskfs. osd-xfs does implement fallocate, so the parameter belongs
 * here.
 *
 *   -1  disabled, fallocate returns -EOPNOTSUPP
 *    0  allocate unwritten extents, which is what XFS does natively
 *    1  zero the allocated extents on disk
 */
static ssize_t fallocate_zero_blocks_show(struct kobject *kobj,
					  struct attribute *attr, char *buf)
{
	struct dt_device *dt = container_of(kobj, struct dt_device, dd_kobj);
	struct osd_device *osd = osd_dt_dev(dt);

	LASSERT(osd);
	if (unlikely(osd->od_mnt == NULL))
		return -EINPROGRESS;

	return scnprintf(buf, PAGE_SIZE, "%d\n",
			 osd->od_fallocate_zero_blocks);
}

static ssize_t fallocate_zero_blocks_store(struct kobject *kobj,
					   struct attribute *attr,
					   const char *buffer, size_t count)
{
	struct dt_device *dt = container_of(kobj, struct dt_device, dd_kobj);
	struct osd_device *osd = osd_dt_dev(dt);
	long val;
	int rc;

	LASSERT(osd);
	if (unlikely(osd->od_mnt == NULL))
		return -EINPROGRESS;

	rc = kstrtol(buffer, 0, &val);
	if (rc)
		return rc;

	if (val < -1 || val > 1)
		return -EINVAL;

	osd->od_fallocate_zero_blocks = val;
	return count;
}
LUSTRE_RW_ATTR(fallocate_zero_blocks);

static struct attribute *xfs_attrs[] = {
	&lustre_attr_nonrotational.attr,
	&lustre_attr_read_cache_enable.attr,
	&lustre_attr_writethrough_cache_enable.attr,
	&lustre_attr_fallocate_zero_blocks.attr,
	NULL,
};

ATTRIBUTE_GROUPS(xfs); /* builds xfs_groups from xfs_attrs */

/* Apply only parameters backed by this target's canonical sysfs table. */
int osd_process_param(struct osd_device *osd, struct lustre_cfg *cfg)
{
	struct lustre_cfg_bufs bufs;
	struct lustre_cfg *param;
	int i, j, len;
	ssize_t count;

	if (cfg->lcfg_bufcount < 2)
		return -EINVAL;
	if (!osd->od_dt_dev.dd_kobj.state_in_sysfs)
		return -ENODEV;

	for (i = 1; i < cfg->lcfg_bufcount; i++) {
		const char *prefix = PARAM_OSD;
		char *setting = lustre_cfg_string(cfg, i);
		char *key, *value;

		if (!setting)
			return -EINVAL;
		if (class_match_param(setting, prefix, &key)) {
			prefix = PARAM_OST;
			if (class_match_param(setting, prefix, &key))
				return -EINVAL;
		}
		value = strchr(key, '=');
		if (!value || value == key || !value[1])
			return -EINVAL;
		for (j = 0; xfs_attrs[j]; j++) {
			if (strlen(xfs_attrs[j]->name) == value - key &&
			    !strncmp(xfs_attrs[j]->name, key, value - key))
				break;
		}
		if (!xfs_attrs[j] || !(xfs_attrs[j]->mode & 0222)) {
			CERROR("%s: unsupported persistent parameter '%s'\n",
			       osd_name(osd), setting);
			return -EOPNOTSUPP;
		}
		if (cfg->lcfg_bufcount == 2) {
			count = class_modify_config(cfg, prefix,
						    &osd->od_dt_dev.dd_kobj);
			return count > 0 ? 0 : count < 0 ? count : -EINVAL;
		}

		/*
		 * class_modify_config sums store results for a multi-entry cfg.
		 * Submit one entry at a time so successful writes cannot mask a
		 * later error, and never retry a failed store under another prefix.
		 */
		lustre_cfg_bufs_reset(&bufs, lustre_cfg_string(cfg, 0));
		lustre_cfg_bufs_set_string(&bufs, 1, setting);
		len = lustre_cfg_len(bufs.lcfg_bufcount, bufs.lcfg_buflen);
		OBD_ALLOC(param, len);
		if (!param)
			return -ENOMEM;
		lustre_cfg_init(param, LCFG_PARAM, &bufs);
		count = class_modify_config(param, prefix,
					    &osd->od_dt_dev.dd_kobj);
		OBD_FREE(param, len);
		if (count <= 0)
			return count < 0 ? count : -EINVAL;
	}
	return 0;
}

static struct ldebugfs_vars ldebugfs_osd_obd_vars[] = {
	{ .name = "mntdev",
	  .fops = &osd_mntdev_fops },
	{ .name = "mount_options",
	  .fops = &osd_mount_options_fops },
	{ .name = "fstype",
	  .fops = &osd_fstype_fops },
	{ .name = "commit_stats",
	  .fops = &osd_commit_stats_fops },
	{ .name = "commit_defer",
	  .fops = &osd_commit_defer_fops },
	{ .name = "commit_delay_ms",
	  .fops = &osd_commit_delay_ms_fops },
	{ .name = "oi_scrub",
	  .fops = &osd_scrub_fops },
	{ .name = "reserve",
	  .fops = &osd_reserve_fops },
	{ .name = "flush_buffered",
	  .fops = &osd_flush_buffered_fops },
	{ .name = "reserve_check",
	  .fops = &osd_reserve_check_fops },
	{ NULL }
};

int osd_procfs_init(struct osd_device *osd, const char *name)
{
	struct obd_type *type;
	int rc;

	ENTRY;

	if (osd->od_dt_dev.dd_debugfs_entry != NULL)
		RETURN(0);

	/*
	 * There is no linkage from lu_type to obd_type, and ld_obd is not
	 * populated this early in device setup, so look the type up by name
	 * the way osd-ldiskfs does. Dereferencing ld_obd here is what made
	 * the first version of this oops during mount.
	 */
	type = class_search_type(LUSTRE_OSD_XFS_NAME);
	if (type == NULL) {
		CERROR("%s: cannot find the " LUSTRE_OSD_XFS_NAME " obd type\n",
		       name);
		RETURN(-ENOENT);
	}
	/* drop the reference class_search_type() took */
	kobject_put(&type->typ_kobj);

	/* must be set before dt_tunables_init(), which creates the kobject */
	osd->od_dt_dev.dd_ktype.default_groups = xfs_groups;

	rc = dt_tunables_init(&osd->od_dt_dev, type, name,
			      ldebugfs_osd_obd_vars);
	if (rc) {
		CERROR("%s: cannot setup sysfs / debugfs entry: rc = %d\n",
		       name, rc);
		RETURN(rc);
	}

	/*
	 * osd-xfs.<target>.stats. Only the three cache counters, because
	 * those are the ones a caller can act on and the ones the test suite
	 * reads; latency histograms can follow if something needs them.
	 *
	 * Failure here is not fatal: the target works without statistics.
	 */
	{
		char param[MAX_OBD_NAME * 4];

		scnprintf(param, sizeof(param), "osd-xfs.%s.stats", name);
		osd->od_stats = ldebugfs_stats_alloc(LPROC_OSD_XFS_LAST, param,
					osd->od_dt_dev.dd_debugfs_entry, 0);
		if (osd->od_stats != NULL) {
			lprocfs_counter_init(osd->od_stats,
				LPROC_OSD_XFS_CACHE_ACCESS,
				LPROCFS_CNTR_AVGMINMAX | LPROCFS_TYPE_PAGES,
				"cache_access");
			lprocfs_counter_init(osd->od_stats,
				LPROC_OSD_XFS_CACHE_HIT,
				LPROCFS_CNTR_AVGMINMAX | LPROCFS_TYPE_PAGES,
				"cache_hit");
			lprocfs_counter_init(osd->od_stats,
				LPROC_OSD_XFS_CACHE_MISS,
				LPROCFS_CNTR_AVGMINMAX | LPROCFS_TYPE_PAGES,
				"cache_miss");
		} else {
			CWARN("%s: no statistics\n", name);
		}
	}

	/*
	 * qsd_init() registers the quota slave's parameters under a
	 * proc_dir_entry, so one has to exist even though everything else this
	 * backend exposes is in debugfs. Without it the quota slave works but
	 * is invisible: no quota_slave.info, and no way to see or set its
	 * state per target.
	 */
	if (osd->od_proc_entry == NULL) {
		osd->od_proc_entry = lprocfs_register(name, type->typ_procroot,
						      NULL, &osd->od_dt_dev);
		if (IS_ERR(osd->od_proc_entry)) {
			rc = PTR_ERR(osd->od_proc_entry);
			CWARN("%s: no procfs entry, quota slave parameters will be missing: rc = %d\n",
			      name, rc);
			osd->od_proc_entry = NULL;
			rc = 0;
		}
	}

	RETURN(0);
}

void osd_procfs_fini(struct osd_device *osd)
{
	if (osd->od_stats != NULL) {
		lprocfs_stats_free(&osd->od_stats);
		osd->od_stats = NULL;
	}

	if (osd->od_proc_entry != NULL) {
		lprocfs_remove(&osd->od_proc_entry);
		osd->od_proc_entry = NULL;
	}

	dt_tunables_fini(&osd->od_dt_dev);
}
