// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * OI scrub for osd-xfs: rebuild the object index from the namespace.
 *
 * The object index is oi/<hex-fid> -> "ino:gen", a directory of symlinks. It
 * covers the objects whose location cannot be derived from their FID: the
 * local files, the llog catalogues, quota files, and anything created before
 * its name was known. If it is damaged those objects become unreachable even
 * though the data is intact, and there is nothing else on disk that maps a FID
 * to an inode.
 *
 * osd-ldiskfs rebuilds its equivalent by walking the ext4 inode table
 * directly, which is 3,628 lines of osd_scrub.c and needs the on-disk format.
 * That is not available here, and XFS's bulkstat is not reachable from a
 * module.
 *
 * It is also not necessary. Every object that needs an index entry has a name:
 * either in the namespace proper, or in oi/stage/ where objects wait for the
 * name Lustre has not supplied yet. So walking the tree finds all of them, and
 * each one carries its own FID in trusted.lma. The index is therefore
 * derivable from the namespace, and the walk needs nothing but readdir,
 * lookup, and getxattr.
 *
 * The one case a namespace walk cannot recover is an inode with no name at
 * all. Those are not this code's problem: XFS's own repair moves them to
 * lost+found, and an object osd-xfs created always has a name, because
 * osd_stage_create() creates it in oi/stage/ in the same XFS transaction as
 * its inode and trusted.lma.
 */

#define DEBUG_SUBSYSTEM S_OSD

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/dcache.h>

#include "osd_internal.h"

/*
 * osd_name() reads the obd name, which is not attached yet when the scrub runs
 * at mount and prints as "(efault)". The service name is set before this.
 */
static inline const char *osd_scrub_name(struct osd_device *dev)
{
	return dev->od_svname[0] ? dev->od_svname : "osd-xfs";
}

#define OSD_SCRUB_BATCH		64
#define OSD_SCRUB_MAX_DEPTH	8

struct osd_scrub_ent {
	char		se_name[NAME_MAX + 1];
	int		se_namelen;
	unsigned int	se_type;
};

struct osd_scrub_dir {
	struct file		*sd_file;
	struct osd_scrub_ent	*sd_ents;
	int			 sd_nents;
	int			 sd_cur;
};

struct osd_scrub_ctx {
	struct dir_context	 ctx;
	struct osd_scrub_dir	*sd;
};

static bool osd_scrub_filldir(struct dir_context *ctx, const char *name,
			      int namelen, loff_t offset, u64 ino,
			      unsigned int d_type)
{
	struct osd_scrub_ctx *sc = container_of(ctx, struct osd_scrub_ctx, ctx);
	struct osd_scrub_dir *sd = sc->sd;
	struct osd_scrub_ent *e;

	/*
	 * No room: return false WITHOUT storing, so the filesystem leaves its
	 * position on this entry and re-emits it next time. Storing and then
	 * refusing duplicates the entry.
	 */
	if (sd->sd_nents >= OSD_SCRUB_BATCH)
		return false;
	if (namelen > NAME_MAX)
		return true;			/* skip, keep going */
	if (namelen == 1 && name[0] == '.')
		return true;
	if (namelen == 2 && name[0] == '.' && name[1] == '.')
		return true;

	e = &sd->sd_ents[sd->sd_nents++];
	memcpy(e->se_name, name, namelen);
	e->se_name[namelen] = '\0';
	e->se_namelen = namelen;
	e->se_type = d_type;

	return true;
}

static int osd_scrub_next(struct osd_scrub_dir *sd,
			  struct osd_scrub_ent **ent)
{
	struct osd_scrub_ctx sc = {
		.ctx.actor = osd_scrub_filldir,
		.sd = sd,
	};
	int rc;

	if (sd->sd_cur < sd->sd_nents) {
		*ent = &sd->sd_ents[sd->sd_cur++];
		return 0;
	}

	sd->sd_nents = 0;
	sd->sd_cur = 0;
	sc.ctx.pos = sd->sd_file->f_pos;

	rc = iterate_dir(sd->sd_file, &sc.ctx);
	if (rc < 0)
		return rc;
	if (sd->sd_nents == 0)
		return 1;			/* end of directory */

	*ent = &sd->sd_ents[sd->sd_cur++];
	return 0;
}

/*
 * Does oi/ already hold a correct entry for @fid -> @inode?
 */
static bool osd_scrub_entry_ok(const struct lu_env *env,
			       struct osd_device *dev,
			       const struct lu_fid *fid,
			       struct inode *inode)
{
	struct dentry *found = NULL;
	bool ok = false;
	int rc;

	rc = osd_oi_lookup(env, dev, fid, &found);
	if (rc == 0 && found != NULL) {
		ok = (d_inode(found) == inode);
		dput(found);
	}

	return ok;
}

/*
 * Check one directory entry, and report whether it is itself a directory that
 * the caller should descend into.
 */
static int osd_scrub_one(const struct lu_env *env, struct osd_device *dev,
			 struct dentry *parent, struct osd_scrub_ent *ent,
			 struct osd_scrub_stats *stats,
			 struct dentry **descend)
{
	struct dentry *dchild;
	struct lu_fid fid;
	struct inode *inode;
	int rc;

	*descend = NULL;

	{
		struct qstr qstr = QSTR_INIT(ent->se_name, ent->se_namelen);

		inode_lock_shared(d_inode(parent));
		dchild = lookup_noperm(&qstr, parent);
		inode_unlock_shared(d_inode(parent));
	}
	if (IS_ERR(dchild))
		return PTR_ERR(dchild);
	if (!d_really_is_positive(dchild)) {
		dput(dchild);
		return 0;
	}

	inode = d_inode(dchild);
	stats->ss_scanned++;

	if (S_ISDIR(inode->i_mode))
		*descend = dget(dchild);

	/*
	 * No LMA means the object is not one of Lustre's, or predates it. Not
	 * an error and not something to index: only an object with a FID can
	 * have an index entry.
	 */
	rc = osd_lma_get(env, inode, &fid);
	if (rc != 0) {
		stats->ss_no_lma++;
		GOTO(out, rc = 0);
	}

	/*
	 * An OST data object is found by a path derived from its FID, so it
	 * has no index entry and must not be given one.
	 */
	if (osd_fid_is_on_ost(&fid)) {
		stats->ss_derivable++;
		GOTO(out, rc = 0);
	}

	if (osd_scrub_entry_ok(env, dev, &fid, inode)) {
		stats->ss_ok++;
		GOTO(out, rc = 0);
	}

	/*
	 * Missing or wrong. osd_oi_insert() replaces an existing entry, so
	 * this covers both a lost entry and one that points at the wrong
	 * inode after a FID was reused.
	 */
	rc = osd_oi_insert(env, dev, &fid, inode);
	if (rc == 0) {
		stats->ss_repaired++;
		CDEBUG(D_LFSCK, "%s: scrub rebuilt oi entry for "DFID
		       " -> ino %lu\n", osd_name(dev), PFID(&fid),
		       inode->i_ino);
	} else {
		stats->ss_failed++;
		CWARN("%s: scrub cannot rebuild oi entry for "DFID": rc = %d\n",
		      osd_name(dev), PFID(&fid), rc);
		/* keep going: one bad object should not abort the scrub */
		rc = 0;
	}
	EXIT;
out:
	dput(dchild);
	return rc;
}

/*
 * Walk @root and everything under it. Iterative rather than recursive: the
 * depth is bounded but the stack in a filesystem thread is not generous, and
 * a directory loop on a damaged filesystem would otherwise run off the end of
 * it.
 */
static int osd_scrub_walk(const struct lu_env *env, struct osd_device *dev,
			  struct dentry *root, struct osd_scrub_stats *stats)
{
	struct osd_scrub_dir *stack;
	struct dentry **dents;
	int depth = 0;
	int rc = 0;
	int i;

	OBD_ALLOC_PTR_ARRAY(stack, OSD_SCRUB_MAX_DEPTH);
	if (stack == NULL)
		return -ENOMEM;
	OBD_ALLOC_PTR_ARRAY(dents, OSD_SCRUB_MAX_DEPTH);
	if (dents == NULL) {
		OBD_FREE_PTR_ARRAY(stack, OSD_SCRUB_MAX_DEPTH);
		return -ENOMEM;
	}

	for (i = 0; i < OSD_SCRUB_MAX_DEPTH; i++) {
		OBD_ALLOC_PTR_ARRAY_LARGE(stack[i].sd_ents, OSD_SCRUB_BATCH);
		if (stack[i].sd_ents == NULL)
			GOTO(out, rc = -ENOMEM);
	}

	dents[0] = dget(root);
	stack[0].sd_file = dentry_open(&(struct path){ .mnt = dev->od_mnt,
						       .dentry = dents[0] },
				       O_RDONLY | O_DIRECTORY | O_NOATIME,
				       current_cred());
	if (IS_ERR(stack[0].sd_file)) {
		rc = PTR_ERR(stack[0].sd_file);
		stack[0].sd_file = NULL;
		dput(dents[0]);
		GOTO(out, rc);
	}

	while (depth >= 0) {
		struct osd_scrub_ent *ent = NULL;
		struct dentry *sub = NULL;

		if (unlikely(dev->od_scrub_stop))
			GOTO(out, rc = -EINTR);

		rc = osd_scrub_next(&stack[depth], &ent);
		if (rc < 0)
			GOTO(out, rc);
		if (rc == 1) {
			/* done with this level */
			fput(stack[depth].sd_file);
			stack[depth].sd_file = NULL;
			dput(dents[depth]);
			dents[depth] = NULL;
			depth--;
			rc = 0;
			continue;
		}

		rc = osd_scrub_one(env, dev, dents[depth], ent, stats, &sub);
		if (rc != 0) {
			if (sub != NULL)
				dput(sub);
			GOTO(out, rc);
		}

		if (sub == NULL)
			continue;

		/*
		 * Do not descend into oi/ itself. It is the thing being
		 * rebuilt, its entries are symlinks rather than objects, and
		 * walking it while inserting into it is asking for trouble.
		 *
		 * Nor into O/. Everything under it is an OST data object,
		 * found by a path derived from its FID and therefore never
		 * indexed -- so scanning it can only ever conclude "needs no
		 * entry". Skipping it is what makes this cheap enough to run
		 * at every mount: O/ holds every data object on the target,
		 * millions of them on a real OST, while the objects that do
		 * need an index entry number in the tens.
		 */
		if (dents[depth] == dev->od_root_dentry &&
		    (strcmp(ent->se_name, OSD_OI_DIRNAME) == 0 ||
		     strcmp(ent->se_name, "O") == 0)) {
			stats->ss_skipped_dirs++;
			dput(sub);
			continue;
		}

		if (depth + 1 >= OSD_SCRUB_MAX_DEPTH) {
			stats->ss_too_deep++;
			dput(sub);
			continue;
		}

		depth++;
		dents[depth] = sub;
		stack[depth].sd_nents = 0;
		stack[depth].sd_cur = 0;
		stack[depth].sd_file =
			dentry_open(&(struct path){ .mnt = dev->od_mnt,
						    .dentry = sub },
				    O_RDONLY | O_DIRECTORY | O_NOATIME,
				    current_cred());
		if (IS_ERR(stack[depth].sd_file)) {
			rc = PTR_ERR(stack[depth].sd_file);
			stack[depth].sd_file = NULL;
			dput(sub);
			dents[depth] = NULL;
			depth--;
			/* an unreadable directory is not fatal */
			stats->ss_failed++;
			rc = 0;
		}
	}
	EXIT;
out:
	for (i = 0; i < OSD_SCRUB_MAX_DEPTH; i++) {
		if (stack[i].sd_file != NULL)
			fput(stack[i].sd_file);
		if (dents[i] != NULL)
			dput(dents[i]);
		if (stack[i].sd_ents != NULL)
			OBD_FREE_PTR_ARRAY_LARGE(stack[i].sd_ents,
						 OSD_SCRUB_BATCH);
	}
	OBD_FREE_PTR_ARRAY(dents, OSD_SCRUB_MAX_DEPTH);
	OBD_FREE_PTR_ARRAY(stack, OSD_SCRUB_MAX_DEPTH);

	return rc;
}

/*
 * Rebuild the object index from the namespace.
 */
int osd_scrub_run(const struct lu_env *env, struct osd_device *dev)
{
	struct osd_scrub_stats stats = { 0 };
	ktime_t start = ktime_get();
	int rc;

	ENTRY;

	if (dev->od_oi == NULL)
		RETURN(-ENOENT);

	mutex_lock(&dev->od_scrub_mutex);
	if (dev->od_scrub_running) {
		mutex_unlock(&dev->od_scrub_mutex);
		RETURN(-EALREADY);
	}
	dev->od_scrub_running = 1;
	dev->od_scrub_stop = 0;
	mutex_unlock(&dev->od_scrub_mutex);

	LCONSOLE_INFO("%s: OI scrub started\n", osd_scrub_name(dev));

	rc = osd_scrub_walk(env, dev, dev->od_root_dentry, &stats);

	/*
	 * oi/stage/ holds objects that were created before Lustre supplied a
	 * name, so they are not reachable from the namespace and the walk
	 * above skipped the whole of oi/. They still need index entries.
	 */
	if (rc == 0 && dev->od_oi->oi_stage != NULL)
		rc = osd_scrub_walk(env, dev, dev->od_oi->oi_stage, &stats);

	stats.ss_time_ms = ktime_ms_delta(ktime_get(), start);

	mutex_lock(&dev->od_scrub_mutex);
	dev->od_scrub_running = 0;
	dev->od_scrub_last = stats;
	dev->od_scrub_last.ss_rc = rc;
	mutex_unlock(&dev->od_scrub_mutex);

	LCONSOLE_INFO("%s: OI scrub %s: %llu scanned, %llu already correct, %llu rebuilt, %llu without an LMA, %llu found by path, %llu failed, in %llu ms\n",
		      osd_scrub_name(dev), rc == 0 ? "complete" : "aborted",
		      stats.ss_scanned, stats.ss_ok, stats.ss_repaired,
		      stats.ss_no_lma, stats.ss_derivable, stats.ss_failed,
		      stats.ss_time_ms);

	RETURN(rc);
}

static int osd_scrub_thread(void *arg)
{
	struct osd_device *dev = arg;
	struct lu_env env;
	int rc;

	rc = lu_env_init(&env, LCT_LOCAL | LCT_DT_THREAD);
	if (rc != 0) {
		CERROR("%s: scrub cannot init an env: rc = %d\n",
		       osd_name(dev), rc);
		goto out;
	}

	osd_scrub_run(&env, dev);
	lu_env_fini(&env);
out:
	mutex_lock(&dev->od_scrub_mutex);
	dev->od_scrub_task = NULL;
	mutex_unlock(&dev->od_scrub_mutex);

	return 0;
}

int osd_scrub_start(struct osd_device *dev)
{
	struct task_struct *task;
	int rc = 0;

	mutex_lock(&dev->od_scrub_mutex);
	if (dev->od_scrub_running || dev->od_scrub_task != NULL) {
		mutex_unlock(&dev->od_scrub_mutex);
		return -EALREADY;
	}
	task = kthread_run(osd_scrub_thread, dev, "osd_xfs_scrub");
	if (IS_ERR(task)) {
		rc = PTR_ERR(task);
		task = NULL;
	}
	dev->od_scrub_task = task;
	mutex_unlock(&dev->od_scrub_mutex);

	return rc;
}

void osd_scrub_stop(struct osd_device *dev)
{
	mutex_lock(&dev->od_scrub_mutex);
	dev->od_scrub_stop = 1;
	mutex_unlock(&dev->od_scrub_mutex);

	/*
	 * The walk checks od_scrub_stop between entries, so it exits within
	 * one object rather than one directory.
	 */
	while (dev->od_scrub_task != NULL)
		schedule_timeout_uninterruptible(HZ / 10);
}
