// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Mount helper plugin for the XFS OSD (osd-xfs).
 *
 * Unlike ldiskfs, XFS has no userspace library that can create and write a
 * file in an unmounted filesystem the way libext2fs can, and unlike ZFS it has
 * no property store to hang the Lustre disk data off. So the server config is
 * kept exactly where ldiskfs keeps it -- CONFIGS/mountdata in the target
 * filesystem -- and this plugin mounts the filesystem briefly to read or write
 * it. That keeps the on-disk layout familiar to anyone who has debugged an
 * ldiskfs target, and keeps tunefs.lustre and l_tunedisk working unchanged.
 *
 * Author: generated for the osd-xfs port
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/types.h>
#include <linux/lustre/lustre_disk.h>
#include <linux/lustre/lustre_param.h>

#include "mount_utils.h"

#define XFS_MOUNT_TRIES	15
#define XFS_MOUNT_TMPL	"/tmp/lustre_xfs_XXXXXX"

/*
 * Minimum sane target size. Lustre puts last_rcvd, reply_data, the O/ tree and
 * the llog catalogs on every OST; below this XFS itself refuses to make a
 * filesystem with a usable log.
 */
#define XFS_MIN_TARGET_KB	(300 * 1024)

/* xfs_sb.sb_fname is 12 bytes, not NUL-terminated when full. */
#define XFS_LABEL_MAX		12

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int xfs_mount_tmp(const char *dev, char *mntpt, size_t mntpt_len,
			 int read_only)
{
	char tmpl[] = XFS_MOUNT_TMPL;
	char *dir;
	unsigned long flags = 0;
	int rc;
	int i;

	dir = mkdtemp(tmpl);
	if (dir == NULL) {
		rc = -errno;
		fprintf(stderr, "%s: cannot create temp mountpoint: %s\n",
			progname, strerror(-rc));
		return rc;
	}

	if (read_only)
		flags |= MS_RDONLY;

	/*
	 * Retry on EBUSY.
	 *
	 * Every read or write of the Lustre disk data mounts the target
	 * briefly, and mount.lustre does that immediately after the previous
	 * mount of the same device was torn down. The block device can still
	 * be held for a moment by the outgoing superblock, which surfaces as
	 * "cannot mount /dev/vdb as xfs: Device or resource busy" and then, one
	 * caller up, as the far more misleading "has not been formatted with
	 * mkfs.lustre".
	 */
	rc = 0;
	for (i = 0; i < XFS_MOUNT_TRIES; i++) {
		if (mount(dev, dir, "xfs", flags, NULL) == 0) {
			rc = 0;
			break;
		}
		rc = -errno;
		if (rc != -EBUSY)
			break;
		usleep(200000);
	}
	if (rc != 0) {
		fprintf(stderr, "%s: cannot mount %s as xfs: %s\n",
			progname, dev, strerror(-rc));
		rmdir(dir);
		return rc;
	}

	if (snprintf(mntpt, mntpt_len, "%s", dir) >= (int)mntpt_len) {
		umount(dir);
		rmdir(dir);
		return -E2BIG;
	}
	return 0;
}

static void xfs_umount_tmp(const char *mntpt)
{
	if (mntpt == NULL || mntpt[0] == '\0')
		return;
	if (umount(mntpt) != 0)
		fprintf(stderr, "%s: warning: failed to unmount %s: %s\n",
			progname, mntpt, strerror(errno));
	rmdir(mntpt);
}

/* ------------------------------------------------------------------ */
/* ldd read / write                                                   */
/* ------------------------------------------------------------------ */

int xfs_write_ldd(struct mkfs_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	char mntpt[PATH_MAX];
	char filepnm[PATH_MAX + 64];
	char *dev;
	FILE *filep;
	int rc, ret;

	dev = mop->mo_device;
	if (mop->mo_flags & MO_IS_LOOP)
		dev = mop->mo_loopdev;

	rc = xfs_mount_tmp(dev, mntpt, sizeof(mntpt), 0);
	if (rc)
		return rc;

	snprintf(filepnm, sizeof(filepnm), "%s/%s", mntpt, MOUNT_CONFIGS_DIR);
	if (mkdir(filepnm, 0777) != 0 && errno != EEXIST) {
		rc = -errno;
		fprintf(stderr, "%s: failed to create %s: %s\n",
			progname, filepnm, strerror(-rc));
		goto out;
	}

	snprintf(filepnm, sizeof(filepnm), "%s/%s", mntpt, MOUNT_DATA_FILE);
	filep = fopen(filepnm, "w");
	if (filep == NULL) {
		rc = -errno;
		fprintf(stderr, "%s: unable to create %s: %s\n",
			progname, filepnm, strerror(-rc));
		goto out;
	}

	vprint("Writing %s\n", MOUNT_DATA_FILE);
	ret = fwrite(ldd, sizeof(*ldd), 1, filep);
	if (ret != 1) {
		rc = -errno;
		fprintf(stderr, "%s: unable to write to %s: %s\n",
			progname, filepnm, strerror(-rc));
		fclose(filep);
		goto out;
	}
	if (fflush(filep) != 0 || fsync(fileno(filep)) != 0) {
		rc = -errno;
		fclose(filep);
		goto out;
	}
	fclose(filep);
	rc = 0;
out:
	xfs_umount_tmp(mntpt);
	return rc;
}

int xfs_read_ldd(char *dev, struct lustre_disk_data *ldd)
{
	char mntpt[PATH_MAX];
	char filepnm[PATH_MAX + 64];
	FILE *filep;
	int rc;

	rc = xfs_mount_tmp(dev, mntpt, sizeof(mntpt), 1);
	if (rc)
		return rc;

	snprintf(filepnm, sizeof(filepnm), "%s/%s", mntpt, MOUNT_DATA_FILE);
	filep = fopen(filepnm, "r");
	if (filep == NULL) {
		rc = -errno;
		goto out;
	}

	if (fread(ldd, sizeof(*ldd), 1, filep) != 1) {
		rc = -errno ?: -EINVAL;
		fclose(filep);
		goto out;
	}
	fclose(filep);
	rc = 0;
out:
	xfs_umount_tmp(mntpt);
	return rc;
}

int xfs_erase_ldd(struct mkfs_opts *mop, char *param)
{
	/* erase_param() already removed it from ldd_params; write_ldd persists it. */
	return 0;
}

void xfs_print_ldd_params(struct mkfs_opts *mop)
{
	printf("Parameters:%s\n", mop->mo_ldd.ldd_params);
}

/* ------------------------------------------------------------------ */
/* detection                                                          */
/* ------------------------------------------------------------------ */

/*
 * Report whether the device holds an XFS filesystem that has been formatted as
 * a Lustre target. Probing the magic without mounting keeps "is this even XFS"
 * cheap; only then do we pay for a mount to look for CONFIGS/mountdata.
 */
static int xfs_is_xfs(const char *dev)
{
	unsigned char buf[4];
	int fd, rc = 0;

	fd = open(dev, O_RDONLY);
	if (fd < 0)
		return 0;
	if (read(fd, buf, sizeof(buf)) == sizeof(buf) &&
	    buf[0] == 'X' && buf[1] == 'F' && buf[2] == 'S' && buf[3] == 'B')
		rc = 1;
	close(fd);
	return rc;
}

int xfs_is_lustre(char *dev, enum ldd_mount_type *mount_type)
{
	struct lustre_disk_data ldd;

	if (!xfs_is_xfs(dev))
		return 0;

	memset(&ldd, 0, sizeof(ldd));
	if (xfs_read_ldd(dev, &ldd) != 0)
		return 0;

	if (ldd.ldd_magic != LDD_MAGIC)
		return 0;

	*mount_type = LDD_MT_XFS;
	return 1;
}

/* ------------------------------------------------------------------ */
/* format                                                             */
/* ------------------------------------------------------------------ */

int xfs_make_lustre(struct mkfs_opts *mop)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	char mkfs_cmd[PATH_MAX * 2];
	char label[64];
	char size_opt[32] = "";
	char *dev;
	int rc;

	if (!(mop->mo_flags & MO_FORCEFORMAT) &&
	    xfs_is_xfs(mop->mo_device)) {
		enum ldd_mount_type mt;

		if (xfs_is_lustre(mop->mo_device, &mt)) {
			fprintf(stderr,
				"%s: %s is already a Lustre target; use --reformat to overwrite\n",
				progname, mop->mo_device);
			return -EEXIST;
		}
	}

	dev = mop->mo_device;
	if (mop->mo_flags & MO_IS_LOOP)
		dev = mop->mo_loopdev;

	if (mop->mo_device_kb != 0 && mop->mo_device_kb < XFS_MIN_TARGET_KB) {
		fprintf(stderr,
			"%s: %s is too small for a Lustre target (%llu kB, need at least %u kB)\n",
			progname, dev,
			(unsigned long long)mop->mo_device_kb,
			XFS_MIN_TARGET_KB);
		return -EINVAL;
	}

	/*
	 * XFS labels are capped at 12 characters, shorter than a Lustre
	 * service name, so store as much of the TAIL as fits. The tail is
	 * what distinguishes targets: "lustre-OST0000" and "lustre-OST0001"
	 * share their first 12 characters, so a head truncation labels every
	 * target on a node identically and blkid can no longer tell them
	 * apart. The authoritative name still lives in CONFIGS/mountdata;
	 * this is what tools that read the label see.
	 */
	{
		size_t len = strlen(ldd->ldd_svname);
		const char *tail = ldd->ldd_svname;

		if (len > XFS_LABEL_MAX)
			tail += len - XFS_LABEL_MAX;
		snprintf(label, sizeof(label), "%s", tail);
	}

	/*
	 * -i size=1024
	 *     osd-xfs keeps trusted.lma (24 bytes) and trusted.fid (up to 56)
	 *     on every OST object. A 1 KiB inode leaves both in the shortform
	 *     attribute fork, so an xattr set never has to spill to a remote
	 *     block and never has to roll a transaction. This is what makes
	 *     the OST path fit inside one XFS transaction.
	 * -m crc=1,finobt=1
	 *     v5 format: needed for the per-inode generation counter that the
	 *     OSD uses to validate FID -> inode mappings.
	 * -n size=8192
	 *     the O/<seq>/dN/ object directories get large; bigger directory
	 *     blocks keep them in fewer extents.
	 * -l lazy-count=1
	 *     reduces superblock contention on a metadata-heavy target.
	 */
	/*
	 * --device-size has to reach mkfs.xfs, or it is silently ignored and
	 * the target claims the whole device. That is not cosmetic: the OFD
	 * scales its client grant with target size, so a 100 GB filesystem
	 * where the caller asked for 18 GB hands out grants far above the
	 * cap sanity 64d checks.
	 *
	 * It goes in as "-d size=<n>k". A bare size argument after the device
	 * is the mkfs.xfs syntax for a block count and mkfs rejects it with a
	 * usage message when given a unit suffix.
	 */
	if (mop->mo_device_kb != 0)
		snprintf(size_opt, sizeof(size_opt), "-d size=%lluk",
			 (unsigned long long)mop->mo_device_kb);

	snprintf(mkfs_cmd, sizeof(mkfs_cmd),
		 "mkfs.xfs -f -L \"%s\" -i size=1024 -m crc=1,finobt=1 "
		 "-n size=8192 -l lazy-count=1 %s %s %s",
		 label,
		 size_opt,
		 (mop->mo_mkfsopts[0] != '\0') ? mop->mo_mkfsopts : "",
		 dev);

	vprint("formatting backing filesystem xfs on %s\n", dev);
	vprint("\ttarget name   %s\n", ldd->ldd_svname);
	vprint("\toptions       %s\n", mop->mo_mkfsopts);
	vprint("mkfs_cmd = %s\n", mkfs_cmd);

	rc = run_command(mkfs_cmd, sizeof(mkfs_cmd));
	if (rc) {
		fprintf(stderr, "%s: mkfs.xfs failed on %s (%d)\n",
			progname, dev, rc);
		return rc;
	}
	return 0;
}

int xfs_prepare_lustre(struct mkfs_opts *mop, char *wanted_mountopts,
		       size_t len)
{
	/*
	 * osd-xfs asks for the backing filesystem to be mounted without
	 * automatic timestamp updates it does not control, and without
	 * discard (Lustre issues its own).
	 */
	if (wanted_mountopts != NULL && len > 0)
		wanted_mountopts[0] = '\0';
	return 0;
}

int xfs_fix_mountopts(struct mkfs_opts *mop, char *mountopts, size_t len)
{
	return 0;
}

int xfs_tune_lustre(char *dev, struct mount_opts *mop)
{
	/* Keep kernel scheduler defaults; no optional block-device tuning here. */
	return 0;
}

int xfs_label_lustre(struct mount_opts *mop)
{
	/*
	 * Nothing to do. mount.lustre calls this after the target is already
	 * mounted, and xfs_admin refuses to relabel a mounted filesystem
	 * ("contains a mounted filesystem ... couldn't initialize XFS
	 * library"). The label is written once by mkfs.xfs, and in any case an
	 * XFS label is 12 characters, too short for a Lustre service name --
	 * the authoritative name lives in CONFIGS/mountdata.
	 */
	return 0;
}

int xfs_label_read(char *dev, struct lustre_disk_data *ldd)
{
	/*
	 * The label is only 12 characters, too short to hold a full Lustre
	 * service name, so the authoritative name comes from
	 * CONFIGS/mountdata. Required by the plugin loader, hence present.
	 */
	return xfs_read_ldd(dev, ldd);
}

int xfs_rename_fsname(struct mkfs_opts *mop, const char *oldname)
{
	struct lustre_disk_data *ldd = &mop->mo_ldd;
	int rc;

	rc = xfs_write_ldd(mop);
	if (rc)
		return rc;

	vprint("Renamed %s to %s\n", oldname, ldd->ldd_fsname);
	return 0;
}

int xfs_enable_quota(struct mkfs_opts *mop)
{
	/* Accounting is enabled by osd_mount(); there is no offline toggle. */
	fprintf(stderr, "%s: XFS quota accounting is enabled when osd-xfs mounts the target; --quota is not an offline XFS operation\n",
		progname);
	return -EOPNOTSUPP;
}

int xfs_init(void)
{
	return 0;
}

void xfs_fini(void)
{
}

#ifndef PLUGIN_DIR
struct module_backfs_ops xfs_ops = {
	.init			= xfs_init,
	.fini			= xfs_fini,
	.read_ldd		= xfs_read_ldd,
	.write_ldd		= xfs_write_ldd,
	.erase_ldd		= xfs_erase_ldd,
	.print_ldd_params	= xfs_print_ldd_params,
	.is_lustre		= xfs_is_lustre,
	.make_lustre		= xfs_make_lustre,
	.prepare_lustre		= xfs_prepare_lustre,
	.tune_lustre		= xfs_tune_lustre,
	.label_lustre		= xfs_label_lustre,
	.label_read		= xfs_label_read,
	.enable_quota		= xfs_enable_quota,
	.rename_fsname		= xfs_rename_fsname,
	.fix_mountopts		= xfs_fix_mountopts,
};
#endif /* PLUGIN_DIR */
