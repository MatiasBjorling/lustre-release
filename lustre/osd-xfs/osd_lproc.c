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

static struct attribute *xfs_attrs[] = {
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
	{ .name = "flush_buffered",
	  .fops = &osd_flush_buffered_fops },
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

	RETURN(0);
}

void osd_procfs_fini(struct osd_device *osd)
{
	dt_tunables_fini(&osd->od_dt_dev);
}
