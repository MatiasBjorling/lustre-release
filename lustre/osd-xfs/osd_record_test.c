// SPDX-License-Identifier: GPL-2.0
/*
 * Destructive DT regression probe for a disposable, standalone osd-xfs device.
 * Never attach this to a service stack: LAST_RECV_OID is the real record FID.
 * Setup/verify pairs deliberately leave their fixtures on disk for an OSD
 * detach/reattach or a power cut controlled by the external runner.
 */
#define pr_fmt(fmt) "osd_record_test: " fmt

#include <linux/blkdev.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/slab.h>

#include "osd_internal.h"

#define RT_BASE 8192
#define RT_SLOT 128
#define RT_CHUNK 512
#define RT_EXTENT 1536
#define RT_KEEP 193
#define RT_MARKER "trusted.osdrec.truncate"
#define RT_CHUNK_NAME "trusted.osdrec.16"
#define RT_SEQ_BASE 0x7fff000000000000ULL
#define RT_SEQ_COUNT (2 * OSD_DIR_BATCH + 1)
#define RT_SEQ_OWNER "trusted.osd_record_test.seq"

static char *target;
module_param(target, charp, 0400);
MODULE_PARM_DESC(target, "Disposable standalone osd-xfs OBD name (required)");
static char *mode = "basic";
module_param(mode, charp, 0400);
MODULE_PARM_DESC(mode, "basic, growth/truncate/malformed/legacy/legacy-xattr/migration/unknown/preseed/preseed-xattr/last-seq/bulk-setup or -verify, last-seq-invalid, config, fault, stop-fault");
static unsigned int rounds = 128;
module_param(rounds, uint, 0400);
MODULE_PARM_DESC(rounds, "Concurrent shared-chunk or bulk transaction rounds, 1..4096");

struct rt_fixture {
	struct lu_env env;
	struct dt_device *dev;
	struct lu_device top;
	struct dt_object *obj;
	u8 expected[RT_EXTENT];
	u8 actual[RT_EXTENT];
};

struct rt_marker {
	__le64 size;
	__le64 previous_size;
};

/* OSD objects are lower slices: only a top slice owns lu_object_header. */
struct rt_object {
	struct lu_object_header header;
	struct lu_object lu;
};

static int rt_object_init(const struct lu_env *env, struct lu_object *obj,
			  const struct lu_object_conf *conf)
{
	struct rt_fixture *f = container_of(obj->lo_dev, struct rt_fixture, top);
	struct lu_device *under = &f->dev->dd_lu_dev;
	struct lu_object *below;

	below = under->ld_ops->ldo_object_alloc(env, obj->lo_header, under);
	if (IS_ERR_OR_NULL(below))
		return below ? PTR_ERR(below) : -ENOMEM;
	lu_object_add(obj, below);
	return 0;
}

static void rt_object_free(const struct lu_env *env, struct lu_object *obj)
{
	struct rt_object *top = container_of(obj, struct rt_object, lu);

	lu_object_fini(obj);
	lu_object_header_fini(&top->header);
	kfree_rcu(top, header.loh_rcu);
}

static const struct lu_object_operations rt_object_ops = {
	.loo_object_init = rt_object_init,
	.loo_object_free = rt_object_free,
};

static struct lu_object *rt_object_alloc(const struct lu_env *env,
				       const struct lu_object_header *header,
				       struct lu_device *dev)
{
	struct rt_object *top;

	if (header)
		return NULL;
	top = kzalloc(sizeof(*top), GFP_NOFS);
	if (!top)
		return NULL;
	lu_object_header_init(&top->header);
	lu_object_init(&top->lu, &top->header, dev);
	lu_object_add_top(&top->header, &top->lu);
	top->lu.lo_ops = &rt_object_ops;
	return &top->lu;
}

static const struct lu_device_operations rt_device_ops = {
	.ldo_object_alloc = rt_object_alloc,
};

static const struct lu_device_type_operations rt_type_ops;
static struct lu_device_type rt_type = {
	.ldt_name = "osd-record-test",
	.ldt_ops = &rt_type_ops,
	.ldt_ctx_tags = LCT_LOCAL | LCT_DT_THREAD,
};

static void rt_pattern(u8 *buf, size_t len, unsigned int seed)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = 1 + (i * 37 + seed * 19 + (i >> 7)) % 255;
}

static int rt_stop(const struct lu_env *env, struct dt_device *dev,
		   struct thandle *th, int rc)
{
	int stop;

	th->th_result = rc;
	stop = dt_trans_stop(env, dev, th);
	return rc ? rc : stop;
}

static int rt_write(const struct lu_env *env, struct dt_device *dev,
		    struct dt_object *obj, void *data, size_t len, loff_t off)
{
	struct lu_buf buf = { .lb_buf = data, .lb_len = len };
	struct thandle *th;
	loff_t pos = off;
	ssize_t n;
	int rc;

	th = dt_trans_create(env, dev);
	if (IS_ERR(th))
		return PTR_ERR(th);
	th->th_sync = 1;
	rc = dt_declare_write(env, obj, &buf, pos, th);
	if (!rc)
		rc = dt_trans_start_local(env, dev, th);
	if (!rc) {
		/* No external object lock: exercise the backend's own chunk RMW. */
		n = dt_write(env, obj, &buf, &pos, th);
		rc = n < 0 ? n : n == len && pos == off + len ? 0 : -EIO;
	}
	return rt_stop(env, dev, th, rc);
}

static int rt_resize(struct rt_fixture *f, u64 size, bool punch)
{
	struct lu_attr attr = { .la_valid = LA_SIZE, .la_size = size };
	struct thandle *th;
	int rc;

	th = dt_trans_create(&f->env, f->dev);
	if (IS_ERR(th))
		return PTR_ERR(th);
	th->th_sync = 1;
	rc = dt_declare_attr_set(&f->env, f->obj, &attr, th);
	if (!rc && punch)
		rc = dt_declare_punch(&f->env, f->obj, size,
				      OBD_OBJECT_EOF, th);
	if (!rc)
		rc = dt_trans_start_local(&f->env, f->dev, th);
	if (!rc) {
		dt_write_lock(&f->env, f->obj, 0);
		if (punch)
			rc = dt_punch(&f->env, f->obj, size, OBD_OBJECT_EOF, th);
		if (!rc)
			rc = dt_attr_set(&f->env, f->obj, &attr, th);
		dt_write_unlock(&f->env, f->obj);
	}
	return rt_stop(&f->env, f->dev, th, rc);
}

static int rt_size(struct rt_fixture *f, u64 expected)
{
	struct lu_attr attr = {};
	int rc = dt_attr_get(&f->env, f->obj, &attr);

	if (rc)
		return rc;
	if (!(attr.la_valid & LA_SIZE) || attr.la_size != expected) {
		pr_err("size: expected %llu, got %llu valid=%llx\n",
		       expected, attr.la_size, attr.la_valid);
		return -EUCLEAN;
	}
	return 0;
}

static int rt_compare(struct rt_fixture *f, size_t len)
{
	struct lu_buf buf = { .lb_buf = f->actual, .lb_len = len };
	loff_t pos = RT_BASE;
	int rc;

	memset(f->actual, 0xa5, len);
	rc = dt_read(&f->env, f->obj, &buf, &pos);
	if (rc < 0)
		return rc;
	if (rc != len || pos != RT_BASE + len ||
	    memcmp(f->actual, f->expected, len)) {
		pr_err("record comparison: read=%d expected=%zu position=%lld\n",
		       rc, len, pos);
		return -EUCLEAN;
	}
	return 0;
}

static int rt_reset(struct rt_fixture *f)
{
	struct lr_server_data header = {
		.lsd_feature_incompat = cpu_to_le32(OBD_INCOMPAT_OST |
				OBD_INCOMPAT_COMMON_LR | OBD_INCOMPAT_XFS_REC),
		.lsd_server_size = cpu_to_le32(LR_SERVER_SIZE),
		.lsd_client_start = cpu_to_le32(LR_CLIENT_START),
		.lsd_client_size = cpu_to_le16(LR_CLIENT_SIZE),
	};
	struct lu_attr attr;
	int rc = dt_attr_get(&f->env, f->obj, &attr);

	if (rc)
		return rc;
	if (attr.la_size)
		return rt_resize(f, LR_SERVER_SIZE, false);
	return rt_write(&f->env, f->dev, f->obj, &header, sizeof(header), 0);
}

static int rt_seed(struct rt_fixture *f, size_t len)
{
	int rc;

	rc = rt_reset(f);
	if (rc)
		return rc;
	rt_pattern(f->expected, len, 7);
	return rt_write(&f->env, f->dev, f->obj, f->expected, len, RT_BASE);
}

struct rt_worker {
	struct rt_fixture *f;
	struct task_struct *task;
	struct completion go;
	struct completion done;
	unsigned int slot;
	unsigned int seed;
	bool stop;
	int rc;
};

static int rt_worker_main(void *arg)
{
	struct rt_worker *w = arg;
	struct lu_env env;
	u8 bytes[RT_SLOT];
	int rc;

	rc = lu_env_init(&env, LCT_LOCAL | LCT_DT_THREAD);
	for (;;) {
		wait_for_completion(&w->go);
		if (READ_ONCE(w->stop))
			break;
		w->rc = rc;
		if (!rc) {
			rt_pattern(bytes, sizeof(bytes), w->seed);
			w->rc = rt_write(&env, w->f->dev, w->f->obj, bytes,
					 sizeof(bytes), RT_BASE + w->slot * RT_SLOT);
		}
		complete(&w->done);
	}
	if (!rc)
		lu_env_fini(&env);
	return 0;
}

static int rt_concurrent(struct rt_fixture *f)
{
	struct rt_worker workers[2] = {};
	unsigned int i, round;
	int rc;

	rc = rt_seed(f, RT_CHUNK);
	if (rc)
		return rc;
	for (i = 0; i < ARRAY_SIZE(workers); i++) {
		workers[i].f = f;
		workers[i].slot = i;
		init_completion(&workers[i].go);
		init_completion(&workers[i].done);
		workers[i].task = kthread_run(rt_worker_main, &workers[i],
					     "osdrec/%u", i);
		if (IS_ERR(workers[i].task)) {
			rc = PTR_ERR(workers[i].task);
			workers[i].task = NULL;
			goto out;
		}
	}
	for (round = 0; round < rounds; round++) {
		for (i = 0; i < ARRAY_SIZE(workers); i++) {
			workers[i].seed = 11 + round * 2 + i;
			rt_pattern(f->expected + i * RT_SLOT, RT_SLOT,
				   workers[i].seed);
		}
		for (i = 0; i < ARRAY_SIZE(workers); i++)
			complete(&workers[i].go);
		for (i = 0; i < ARRAY_SIZE(workers); i++) {
			wait_for_completion(&workers[i].done);
			if (workers[i].rc && !rc)
				rc = workers[i].rc;
		}
		if (!rc)
			rc = rt_compare(f, RT_CHUNK);
		if (rc) {
			pr_err("shared chunk failed at round %u: %d\n", round, rc);
			goto out;
		}
	}
	pr_info("PASS shared-chunk: %u concurrent rounds, both neighbors preserved\n",
		rounds);
out:
	for (i = 0; i < ARRAY_SIZE(workers); i++) {
		if (!workers[i].task)
			continue;
		WRITE_ONCE(workers[i].stop, true);
		complete(&workers[i].go);
		kthread_stop(workers[i].task);
	}
	return rc;
}

static int rt_eof(struct rt_fixture *f)
{
	struct lu_buf buf = { .lb_buf = f->actual, .lb_len = RT_SLOT };
	loff_t end = RT_BASE + RT_CHUNK;
	loff_t pos = end - 17;
	int rc;

	memset(f->actual, 0xa5, RT_SLOT);
	rc = dt_read(&f->env, f->obj, &buf, &pos);
	if (rc != 17 || pos != end ||
	    memcmp(f->actual, f->expected + RT_CHUNK - 17, 17) ||
	    memchr_inv(f->actual + 17, 0xa5, RT_SLOT - 17))
		return rc < 0 ? rc : -EUCLEAN;
	pos = end;
	rc = dt_read(&f->env, f->obj, &buf, &pos);
	if (rc || pos != end)
		return rc < 0 ? rc : -EUCLEAN;
	pos = end + 29;
	rc = dt_read(&f->env, f->obj, &buf, &pos);
	if (rc || pos != end + 29)
		return rc < 0 ? rc : -EUCLEAN;
	pos = end - 17;
	rc = dt_record_read(&f->env, f->obj, &buf, &pos);
	if (rc != -EFAULT || pos != end)
		return -EUCLEAN;
	pr_info("PASS EOF: clipped read, exact/beyond EOF and fixed-record short read\n");
	return 0;
}

static int rt_truncate(struct rt_fixture *f, bool punch)
{
	int rc = rt_seed(f, RT_EXTENT);

	if (!rc)
		rc = rt_resize(f, RT_BASE + RT_KEEP, punch);
	if (!rc)
		rc = rt_size(f, RT_BASE + RT_KEEP);
	if (!rc)
		rc = rt_resize(f, RT_BASE + RT_EXTENT, false);
	memset(f->expected + RT_KEEP, 0, RT_EXTENT - RT_KEEP);
	if (!rc)
		rc = rt_compare(f, RT_EXTENT);
	if (!rc)
		rc = rt_size(f, RT_BASE + RT_EXTENT);
	if (!rc)
		pr_info("PASS truncate/reextend (%s): prefix retained, partial and whole chunk tails zero\n",
			punch ? "punch+attr" : "attr");
	return rc;
}

static int rt_setxattr(struct rt_fixture *f, const char *name,
		       void *value, size_t len)
{
	struct lu_buf buf = { .lb_buf = value, .lb_len = len };
	struct thandle *th;
	int rc;

	th = dt_trans_create(&f->env, f->dev);
	if (IS_ERR(th))
		return PTR_ERR(th);
	th->th_sync = 1;
	rc = dt_declare_xattr_set(&f->env, f->obj, NULL, &buf, name, 0, th);
	if (!rc)
		rc = dt_trans_start_local(&f->env, f->dev, th);
	if (!rc)
		rc = dt_xattr_set(&f->env, f->obj, &buf, name, 0, th);
	return rt_stop(&f->env, f->dev, th, rc);
}

/* Construct old on-disk input below DT; verify uses a fresh object lookup. */
static int rt_upgrade_setup(struct rt_fixture *f, bool logged, bool migrating)
{
	struct osd_object *obj = osd_dt_obj(f->obj);
	struct osd_device *osd = osd_obj2dev(obj);
	struct osd_rec_format format = {
		.version = cpu_to_le32(OSD_REC_FORMAT_VERSION),
		.state = cpu_to_le32(OSD_REC_FORMAT_MIGRATING),
	};
	struct lr_server_data header;
	struct file *file;
	loff_t pos;
	unsigned int i;
	int rc = rt_seed(f, RT_CHUNK + RT_KEEP);

	if (rc)
		return rc;
	file = dentry_open(&(struct path){ .mnt = osd->od_mnt,
					 .dentry = obj->oo_dentry },
			    O_RDWR | O_LARGEFILE, current_cred());
	if (IS_ERR(file))
		return PTR_ERR(file);
	pos = 0;
	rc = kernel_read(file, &header, sizeof(header), &pos);
	if (rc != sizeof(header)) {
		rc = rc < 0 ? rc : -EIO;
		goto out;
	}
	header.lsd_feature_incompat &= ~cpu_to_le32(OBD_INCOMPAT_XFS_REC);
	pos = 0;
	rc = kernel_write(file, &header, sizeof(header), &pos);
	if (rc != sizeof(header)) {
		rc = rc < 0 ? rc : -EIO;
		goto out;
	}
	/* Delete every migrated chunk so the legacy mirror must supply gaps
	 * and the short final chunk. Retain only a deliberately newer slot.
	 */
	inode_lock(obj->oo_inode);
	for (i = 0; i < DIV_ROUND_UP(RT_BASE + RT_EXTENT, RT_CHUNK); i++) {
		char name[40];

		snprintf(name, sizeof(name), "trusted.osdrec.%u", i);
		rc = __vfs_removexattr(&nop_mnt_idmap, obj->oo_dentry, name);
		if (rc && rc != -ENODATA)
			break;
		rc = 0;
	}
	if (!rc)
		rc = __vfs_removexattr(&nop_mnt_idmap, obj->oo_dentry,
				       OSD_REC_FORMAT_XATTR);
	if (rc == -ENODATA)
		rc = 0;
	if (!rc && logged) {
		rt_pattern(f->actual, RT_CHUNK, 19);
		rc = __vfs_setxattr(&nop_mnt_idmap, obj->oo_dentry,
				    obj->oo_inode, RT_CHUNK_NAME,
				    f->actual, RT_CHUNK, 0);
	}
	if (!rc && migrating)
		rc = __vfs_setxattr(&nop_mnt_idmap, obj->oo_dentry,
				    obj->oo_inode, OSD_REC_FORMAT_XATTR,
				    &format, sizeof(format), 0);
	inode_unlock(obj->oo_inode);
	if (!rc)
		rc = vfs_fsync(file, 0);
out:
	fput(file);
	if (!rc)
		pr_info("SETUP legacy records: logged=%d migrating=%d partial EOF=%u; reopen before verify\n",
			logged, migrating, RT_KEEP);
	return rc;
}

static int rt_upgrade_verify(struct rt_fixture *f, bool logged)
{
	struct osd_object *obj = osd_dt_obj(f->obj);
	struct osd_device *osd = osd_obj2dev(obj);
	struct osd_rec_format format;
	struct lr_server_data header;
	struct lu_buf buf = { .lb_buf = &format, .lb_len = sizeof(format) };
	struct file *file;
	loff_t pos = 0;
	int rc;

	rt_pattern(f->expected, RT_CHUNK + RT_KEEP, 7);
	if (logged)
		rt_pattern(f->expected, RT_CHUNK, 19);
	rc = rt_size(f, RT_BASE + RT_CHUNK + RT_KEEP);
	if (!rc)
		rc = rt_compare(f, RT_CHUNK + RT_KEEP);
	if (rc)
		return rc;
	rc = dt_xattr_get(&f->env, f->obj, &buf, OSD_REC_FORMAT_XATTR);
	if (rc != sizeof(format) ||
	    le32_to_cpu(format.version) != OSD_REC_FORMAT_VERSION ||
	    le32_to_cpu(format.state) != OSD_REC_FORMAT_READY)
		return rc < 0 ? rc : -EUCLEAN;
	file = dentry_open(&(struct path){ .mnt = osd->od_mnt,
					 .dentry = obj->oo_dentry },
			    O_RDONLY | O_LARGEFILE, current_cred());
	if (IS_ERR(file))
		return PTR_ERR(file);
	rc = kernel_read(file, &header, sizeof(header), &pos);
	fput(file);
	if (rc != sizeof(header) ||
	    !(le32_to_cpu(header.lsd_feature_incompat) & OBD_INCOMPAT_XFS_REC))
		return rc < 0 ? rc : -EUCLEAN;
	buf.lb_buf = &header;
	buf.lb_len = sizeof(header);
	pos = 0;
	rc = dt_record_read(&f->env, f->obj, &buf, &pos);
	if (rc || !(le32_to_cpu(header.lsd_feature_incompat) & OBD_INCOMPAT_XFS_REC))
		return rc ? rc : -EUCLEAN;
	pr_info("PASS upgrade: exact size, authoritative legacy data, partial EOF, both downgrade fences\n");
	return 0;
}

static int rt_check_marker(struct rt_fixture *f, bool present)
{
	struct rt_marker marker;
	struct lu_buf buf = { .lb_buf = &marker, .lb_len = sizeof(marker) };
	int rc = dt_xattr_get(&f->env, f->obj, &buf, RT_MARKER);

	if (!present)
		return rc == -ENODATA ? 0 : rc < 0 ? rc : -EUCLEAN;
	if (rc != sizeof(marker))
		return rc < 0 ? rc : -EUCLEAN;
	pr_info("truncate marker bytes=%*phN size=%llu previous_size=%llu\n",
		(int)sizeof(marker), &marker, le64_to_cpu(marker.size),
		le64_to_cpu(marker.previous_size));
	return le64_to_cpu(marker.size) == RT_BASE + RT_KEEP &&
	       le64_to_cpu(marker.previous_size) == RT_BASE + RT_EXTENT ?
	       0 : -EUCLEAN;
}

static int rt_truncate_setup(struct rt_fixture *f)
{
	struct osd_object *obj = osd_dt_obj(f->obj);
	struct rt_marker marker = {
		.size = cpu_to_le64(RT_BASE + RT_KEEP),
		.previous_size = cpu_to_le64(RT_BASE + RT_EXTENT),
	};
	struct iattr attr = {
		.ia_valid = ATTR_SIZE | ATTR_FORCE,
		.ia_size = RT_BASE + RT_KEEP,
	};
	int rc = rt_seed(f, RT_EXTENT);

	if (!rc)
		rc = rt_setxattr(f, RT_MARKER, &marker, sizeof(marker));
	if (rc)
		return rc;
	/* Construct the dangerous recovery input: stale chunks beyond mirror EOF.
	 * Deliberately bypass DT resize, which would repair the marker itself.
	 */
	inode_lock(obj->oo_inode);
	rc = notify_change(&nop_mnt_idmap, obj->oo_dentry, &attr, NULL);
	inode_unlock(obj->oo_inode);
	if (!rc)
		rc = dt_sync(&f->env, f->dev);
	/* Attribute reads are observational, even with a pending repair. */
	if (!rc)
		rc = rt_size(f, RT_BASE + RT_KEEP);
	if (!rc)
		rc = rt_check_marker(f, true);
	if (!rc)
		pr_info("SETUP truncate: constructed interrupted state, reopen OSD before truncate-verify\n");
	return rc;
}

static int rt_truncate_verify(struct rt_fixture *f)
{
	/* Object initialization, not attr_get, must already have replayed it. */
	int rc = rt_check_marker(f, false);

	if (!rc)
		rc = rt_size(f, RT_BASE + RT_KEEP);
	if (!rc)
		rc = rt_check_marker(f, false);
	if (!rc)
		rc = rt_resize(f, RT_BASE + RT_EXTENT, false);
	rt_pattern(f->expected, RT_EXTENT, 7);
	memset(f->expected + RT_KEEP, 0, RT_EXTENT - RT_KEEP);
	if (!rc)
		rc = rt_compare(f, RT_EXTENT);
	if (!rc)
		pr_info("PASS truncate recovery: marker removed and stale tail zero after reextension\n");
	return rc;
}

static int rt_fence(struct rt_fixture *f, int expected)
{
	struct thandle *th;
	int rc, stop;

	th = dt_trans_create(&f->env, f->dev);
	if (IS_ERR(th)) {
		pr_err("fence transaction allocation failed before start: %ld\n", PTR_ERR(th));
		return PTR_ERR(th);
	}
	rc = dt_trans_start_local(&f->env, f->dev, th);
	th->th_result = rc;
	stop = dt_trans_stop(&f->env, f->dev, th);
	if (rc != expected || stop != expected)
		return -EUCLEAN;
	rc = dt_sync(&f->env, f->dev);
	if (rc != expected)
		return -EUCLEAN;
	pr_info("PASS sticky fence: later start, stop and sync return %d\n", expected);
	return 0;
}

static int rt_malformed_setup(struct rt_fixture *f)
{
	struct lu_buf buf = { .lb_buf = f->actual, .lb_len = sizeof(f->actual) };
	int rc = rt_seed(f, RT_CHUNK);

	if (!rc)
		rc = rt_setxattr(f, RT_CHUNK_NAME, f->expected, 17);
	if (rc)
		return rc;
	rc = dt_xattr_get(&f->env, f->obj, &buf, RT_CHUNK_NAME);
	if (rc != 17 || memcmp(f->actual, f->expected, 17))
		return rc < 0 ? rc : -EUCLEAN;
	pr_info("SETUP malformed: %s length=17; reopen before malformed-verify\n",
		RT_CHUNK_NAME);
	return 0;
}

struct rt_callback {
	struct dt_txn_commit_cb cb;
	unsigned int calls;
	int error;
};

static void rt_commit(struct lu_env *env, struct thandle *th,
		      struct dt_txn_commit_cb *cb, int error)
{
	struct rt_callback *result = container_of(cb, struct rt_callback, cb);

	result->calls++;
	result->error = error;
}

static int rt_fault(struct rt_fixture *f, bool stop_fault)
{
	struct rt_callback result = {};
	struct lu_buf buf = { .lb_buf = f->expected, .lb_len = RT_CHUNK };
	struct thandle *th;
	loff_t pos = 0;
	ssize_t n;
	unsigned long injected = OBD_FAIL_DT_TXN_STOP | CFS_FAIL_ONCE;
	unsigned long observed = 0;
	int rc, sync;

	rt_pattern(f->expected, RT_CHUNK, 23);
	th = dt_trans_create(&f->env, f->dev);
	if (IS_ERR(th))
		return PTR_ERR(th);
	th->th_sync = 1;
	rc = dt_declare_write(&f->env, f->obj, &buf, 0, th);
	if (!rc) {
		/* Local transactions bypass dt_txn_hook_stop and its fault check. */
		if (stop_fault)
			rc = dt_trans_start(&f->env, f->dev, th);
		else
			rc = dt_trans_start_local(&f->env, f->dev, th);
	}
	if (rc)
		return rt_stop(&f->env, f->dev, th, rc);
	INIT_LIST_HEAD(&result.cb.dcb_linkage);
	result.cb.dcb_func = rt_commit;
	strscpy(result.cb.dcb_name, "osdrec-test", sizeof(result.cb.dcb_name));
	rc = dt_trans_cb_add(th, &result.cb);
	if (rc)
		return rt_stop(&f->env, f->dev, th, rc);
	n = dt_write(&f->env, f->obj, &buf, &pos, th);
	if (n != RT_CHUNK || pos != RT_CHUNK) {
		rc = rt_stop(&f->env, f->dev, th, n < 0 ? n : -EIO);
		dt_sync(&f->env, f->dev);
		return rc;
	}
	if (stop_fault) {
		/* Global fault state: run alone in a disposable VM, never alongside
		 * another target or fault injector. Do not overwrite an armed fault.
		 */
		if (cmpxchg(&cfs_fail_loc, 0UL, injected) != 0) {
			rt_stop(&f->env, f->dev, th, 0);
			dt_sync(&f->env, f->dev);
			return -EBUSY;
		}
		rc = rt_stop(&f->env, f->dev, th, 0);
		/* Always restore the original zero state before any assertion. */
		observed = xchg(&cfs_fail_loc, 0UL);
	} else {
		/* The ordinary DT write registered this real inode for writeback. */
		mapping_set_error(osd_dt_obj(f->obj)->oo_inode->i_mapping, -EIO);
		rc = rt_stop(&f->env, f->dev, th, 0);
	}
	/* Drain callbacks before their storage or module text can disappear. */
	sync = dt_sync(&f->env, f->dev);
	if (rc != -EIO || sync != -EIO || result.calls != 1 || result.error != -EIO ||
	    (stop_fault && observed != (injected | CFS_FAILED))) {
		pr_err("fault: stop=%d sync=%d callbacks=%u callback_error=%d\n",
		       rc, sync, result.calls, result.error);
		return -EUCLEAN;
	}
	pr_info("PASS %s error: synchronous stop and commit callback return EIO\n",
		stop_fault ? "transaction-stop hook" : "mapping writeback");
	return rt_fence(f, -EIO);
}

/* Raw namespace setup deliberately does not populate om_seq_list. */
static int rt_seq_entry(struct rt_fixture *f, const char *name, bool create,
			bool directory)
{
	struct osd_device *osd = osd_dt_dev(f->dev);
	struct dentry *root = osd->od_ost_map->om_root, *child;
	struct qstr q = QSTR_INIT(name, strlen(name));
	char owner[32];
	int rc;

	inode_lock(d_inode(root));
	child = lookup_noperm(&q, root);
	if (IS_ERR(child)) {
		rc = PTR_ERR(child);
		goto unlock;
	}
	if (create) {
		if (d_really_is_positive(child)) {
			rc = -EEXIST;
			goto put;
		}
		if (directory) {
			child = ll_vfs_mkdir(&nop_mnt_idmap, d_inode(root), child, 0700);
			if (IS_ERR(child)) {
				rc = PTR_ERR(child);
				goto unlock;
			}
			rc = 0;
		} else {
			rc = vfs_create(&nop_mnt_idmap, child, 0600, NULL);
		}
		if (!rc) {
			inode_lock(d_inode(child));
			rc = __vfs_setxattr(&nop_mnt_idmap, child, d_inode(child),
					    RT_SEQ_OWNER, name, strlen(name), XATTR_CREATE);
			inode_unlock(d_inode(child));
			if (rc) {
				if (directory)
					vfs_rmdir(&nop_mnt_idmap, d_inode(root), child, NULL);
				else
					vfs_unlink(&nop_mnt_idmap, d_inode(root), child);
			}
		}
	} else if (d_really_is_negative(child)) {
		rc = -ENOENT;
	} else {
		rc = __vfs_getxattr(child, d_inode(child), RT_SEQ_OWNER,
				   owner, sizeof(owner));
		if (rc != strlen(name) || memcmp(owner, name, strlen(name))) {
			rc = -EUCLEAN;
			goto put;
		}
		/* Never recursively purge an unexpectedly populated sequence. */
		if (directory)
			rc = vfs_rmdir(&nop_mnt_idmap, d_inode(root), child, NULL);
		else
			rc = vfs_unlink(&nop_mnt_idmap, d_inode(root), child);
	}
put:
	dput(child);
unlock:
	inode_unlock(d_inode(root));
	return rc;
}

static int rt_seq_expect(struct rt_fixture *f, int error, u64 expected)
{
	u64 seq = 0;
	int rc = dt_last_seq_get(&f->env, f->dev, &seq);

	if (rc != error || (!rc && seq != expected)) {
		pr_err("last_seq_get: rc=%d seq=%llx expected rc=%d seq=%llx\n",
		       rc, seq, error, expected);
		return -EUCLEAN;
	}
	return 0;
}

static int rt_last_seq(struct rt_fixture *f)
{
	struct osd_device *osd = osd_dt_dev(f->dev);
	char name[32];
	u64 baseline;
	unsigned int i, created = 0;
	int rc, cleanup;

	if (!osd->od_ost_map)
		return -ENODEV;
	if (!strcmp(mode, "last-seq-verify")) {
		/* Descending removals make every entry the maximum once, so hash
		 * iteration order cannot accidentally hide a truncated scan.
		 */
		for (i = RT_SEQ_COUNT; i; i--) {
			rc = rt_seq_expect(f, 0, RT_SEQ_BASE + i - 1);
			if (rc)
				return rc;
			snprintf(name, sizeof(name), "%llx", RT_SEQ_BASE + i - 1);
			rc = rt_seq_entry(f, name, false, true);
			if (rc)
				return rc;
		}
		rc = dt_sync(&f->env, f->dev);
		if (!rc)
			pr_info("PASS persisted sequence scan: %u directories across batches; owned names removed\n",
				RT_SEQ_COUNT);
		return rc;
	}
	rc = dt_last_seq_get(&f->env, f->dev, &baseline);
	if (rc)
		return rc;
	if (baseline >= RT_SEQ_BASE)
		return -EEXIST;
	if (!strcmp(mode, "last-seq-invalid")) {
		static const struct { const char *name; int error; } bad[] = {
			{ "osd-record-test-invalid", -EINVAL },
			{ "ffffffffffffffff", -EOVERFLOW },
			{ "10000000000000000", -ERANGE },
		};

		/* An invalid regular filename is ignored; the same directory
		 * must instead fail, as must representable/unrepresentable overflow.
		 */
		rc = rt_seq_entry(f, bad[0].name, true, false);
		if (rc)
			return rc;
		rc = rt_seq_expect(f, 0, baseline);
		cleanup = rt_seq_entry(f, bad[0].name, false, false);
		if (rc || cleanup)
			return rc ? rc : cleanup;
		for (i = 0; i < ARRAY_SIZE(bad); i++) {
			rc = rt_seq_entry(f, bad[i].name, true, true);
			if (rc)
				return rc;
			rc = rt_seq_expect(f, bad[i].error, 0);
			cleanup = rt_seq_entry(f, bad[i].name, false, true);
			if (rc || cleanup)
				return rc ? rc : cleanup;
		}
		rc = rt_seq_expect(f, 0, baseline);
		if (!rc)
			pr_info("PASS sequence errors: non-directory ignored, invalid/overflow directories rejected\n");
		return rc;
	}
	for (i = 0; i < RT_SEQ_COUNT; i++) {
		snprintf(name, sizeof(name), "%llx", RT_SEQ_BASE + i);
		rc = rt_seq_entry(f, name, true, true);
		if (rc)
			break;
		created++;
	}
	if (!rc)
		rc = dt_sync(&f->env, f->dev);
	if (!rc)
		rc = rt_seq_expect(f, 0, RT_SEQ_BASE + RT_SEQ_COUNT - 1);
	if (!rc) {
		pr_info("SETUP %u persisted O/<hex-seq> directories; detach/unmount/reopen before last-seq-verify\n",
			RT_SEQ_COUNT);
		return 0;
	}
	while (created) {
		snprintf(name, sizeof(name), "%llx", RT_SEQ_BASE + --created);
		cleanup = rt_seq_entry(f, name, false, true);
		if (cleanup)
			pr_err("sequence cleanup %s: %d\n", name, cleanup);
	}
	return rc;
}

static int rt_config_apply(struct rt_fixture *f, char *first, char *second)
{
	struct lustre_cfg_bufs bufs;
	struct lustre_cfg *cfg;
	struct lu_device *dev = &f->dev->dd_lu_dev;
	int len, rc;

	lustre_cfg_bufs_reset(&bufs, target);
	lustre_cfg_bufs_set_string(&bufs, 1, first);
	if (second)
		lustre_cfg_bufs_set_string(&bufs, 2, second);
	len = lustre_cfg_len(bufs.lcfg_bufcount, bufs.lcfg_buflen);
	cfg = kzalloc(len, GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;
	lustre_cfg_init(cfg, LCFG_PARAM, &bufs);
	rc = dev->ld_ops->ldo_process_config(&f->env, dev, cfg);
	kfree(cfg);
	return rc;
}

static int rt_config(struct rt_fixture *f)
{
	struct kobject *kobj = &f->dev->dd_kobj;
	const struct attribute_group **groups = kobj->ktype->default_groups;
	struct attribute *attr = NULL;
	char *buf, restore[80];
	unsigned int original;
	int i, j, rc, cleanup;
	ssize_t count;

	if (!kobj->state_in_sysfs || !groups || !kobj->ktype->sysfs_ops ||
	    !kobj->ktype->sysfs_ops->show)
		return -ENODEV;
	for (i = 0; groups[i] && !attr; i++)
		for (j = 0; groups[i]->attrs[j]; j++)
			if (!strcmp(groups[i]->attrs[j]->name, "read_cache_enable")) {
				attr = groups[i]->attrs[j];
				break;
			}
	if (!attr)
		return -ENOENT;
	buf = (char *)get_zeroed_page(GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	count = kobj->ktype->sysfs_ops->show(kobj, attr, buf);
	rc = count < 0 ? count : kstrtouint(buf, 0, &original);
	if (rc)
		goto free;
	snprintf(restore, sizeof(restore), "osd.read_cache_enable=%u", original);
	rc = rt_config_apply(f, "osd.read_cache_enable=0", NULL);
	if (!rc) {
		count = kobj->ktype->sysfs_ops->show(kobj, attr, buf);
		rc = count == 2 && !memcmp(buf, "0\n", 2) ? 0 : -EUCLEAN;
	}
	if (!rc) {
		/* A successful first store must not mask the later real error. */
		cleanup = rt_config_apply(f, "ost.read_cache_enable=1",
					  "osd.read_cache_enable=invalid");
		count = kobj->ktype->sysfs_ops->show(kobj, attr, buf);
		rc = cleanup == -EINVAL && count == 2 &&
		     !memcmp(buf, "1\n", 2) ? 0 : -EUCLEAN;
	}
	if (!rc && rt_config_apply(f, "osd.osd_record_test_unknown=1", NULL) !=
		   -EOPNOTSUPP)
		rc = -EUCLEAN;
	if (!rc && rt_config_apply(f, "osd.read_cache_enable=", NULL) != -EINVAL)
		rc = -EUCLEAN;
	if (!rc) {
		count = kobj->ktype->sysfs_ops->show(kobj, attr, buf);
		rc = count == 2 && !memcmp(buf, "1\n", 2) ? 0 : -EUCLEAN;
	}
	cleanup = rt_config_apply(f, restore, NULL);
	if (!rc)
		rc = cleanup;
	if (!rc)
		pr_info("PASS LCFG_PARAM: actual sysfs values, both prefixes, multi-entry error, unknown and invalid; restored %u\n",
			original);
free:
	free_page((unsigned long)buf);
	return rc;
}

static int rt_preseed_setup(struct rt_fixture *f, bool logged)
{
	struct osd_object *obj = osd_dt_obj(f->obj);
	struct osd_device *osd = osd_dt_dev(f->dev);
	struct lr_server_data header;
	struct file *file;
	loff_t pos = 0;
	int rc = rt_upgrade_setup(f, false, true);

	if (rc)
		return rc;
	file = dentry_open(&(struct path){ .mnt = osd->od_mnt,
					 .dentry = obj->oo_dentry },
			    O_RDWR | O_LARGEFILE, current_cred());
	if (IS_ERR(file))
		return PTR_ERR(file);
	rc = kernel_read(file, &header, sizeof(header), &pos);
	if (rc != sizeof(header)) {
		rc = rc < 0 ? rc : -EIO;
		goto out;
	}
	header.lsd_last_transno = cpu_to_le64(41);
	pos = 0;
	rc = kernel_write(file, &header, sizeof(header), &pos);
	if (rc != sizeof(header)) {
		rc = rc < 0 ? rc : -EIO;
		goto out;
	}
	inode_lock(obj->oo_inode);
	if (logged) {
		/* A previously authoritative xattr header must not be replaced
		 * with the older mirror while repairing its downgrade bit.
		 */
		header.lsd_last_transno = cpu_to_le64(73);
		rc = __vfs_setxattr(&nop_mnt_idmap, obj->oo_dentry, obj->oo_inode,
				    "trusted.osdrec.0", &header, sizeof(header), 0);
	} else {
		rc = 0;
	}
	/* Real corruption stops prepare at chunk 1, before any later chunk
	 * can be seeded. This is a deterministic interruption boundary,
	 * not a claim that this test itself performed a power cut.
	 */
	if (!rc)
		rc = __vfs_setxattr(&nop_mnt_idmap, obj->oo_dentry, obj->oo_inode,
				    "trusted.osdrec.1", f->expected, 17, 0);
	inode_unlock(obj->oo_inode);
	if (!rc)
		rc = vfs_fsync(file, 0);
out:
	fput(file);
	if (!rc)
		pr_info("SETUP preseed: MIGRATING, file generation 41, attr0 %s, malformed chunk1 and absent chunk2; reopen before verify\n",
			logged ? "generation 73" : "absent");
	return rc;
}

static int rt_preseed_verify(struct rt_fixture *f, bool logged)
{
	struct osd_device *osd = osd_dt_dev(f->dev);
	struct lr_server_data *mirror = (void *)f->expected;
	struct lr_server_data *authoritative = (void *)f->actual;
	struct osd_rec_format format;
	struct dt_object *obj;
	struct lu_fid fid;
	struct dentry *child, *stage = osd->od_oi->oi_stage;
	struct qstr q;
	struct file *file;
	char name[64];
	loff_t pos = 0;
	int rc;

	lu_local_obj_fid(&fid, LAST_RECV_OID);
	snprintf(name, sizeof(name), "%llx:%x:%x", fid_seq(&fid),
		 fid_oid(&fid), fid_ver(&fid));
	q = (struct qstr)QSTR_INIT(name, strlen(name));
	inode_lock_shared(d_inode(stage));
	child = lookup_noperm(&q, stage);
	inode_unlock_shared(d_inode(stage));
	if (IS_ERR(child))
		return PTR_ERR(child);
	if (d_really_is_negative(child)) {
		dput(child);
		return -ENOENT;
	}
	file = dentry_open(&(struct path){ .mnt = osd->od_mnt, .dentry = child },
			    O_RDONLY | O_LARGEFILE, current_cred());
	dput(child);
	if (IS_ERR(file))
		return PTR_ERR(file);
	child = file->f_path.dentry;
	obj = dt_locate_at(&f->env, f->dev, &fid, &f->top, NULL);
	if (!IS_ERR(obj)) {
		lu_object_put_nocache(&f->env, &obj->do_lu);
		rc = -EUCLEAN;
		goto out;
	}
	rc = PTR_ERR(obj);
	if (rc != -EUCLEAN)
		goto out;
	rc = __vfs_getxattr(child, file_inode(file), OSD_REC_FORMAT_XATTR,
			    &format, sizeof(format));
	if (rc != sizeof(format) ||
	    le32_to_cpu(format.version) != OSD_REC_FORMAT_VERSION ||
	    le32_to_cpu(format.state) != OSD_REC_FORMAT_MIGRATING)
		goto corrupt;
	rc = __vfs_getxattr(child, file_inode(file), "trusted.osdrec.1", NULL, 0);
	if (rc != 17)
		goto corrupt;
	rc = __vfs_getxattr(child, file_inode(file), "trusted.osdrec.2", NULL, 0);
	if (rc != -ENODATA)
		goto corrupt;
	rc = kernel_read(file, mirror, sizeof(*mirror), &pos);
	if (rc != sizeof(*mirror))
		goto corrupt;
	rc = __vfs_getxattr(child, file_inode(file), "trusted.osdrec.0",
			    authoritative, sizeof(*authoritative));
	if (rc != sizeof(*authoritative) ||
	    !(le32_to_cpu(mirror->lsd_feature_incompat) & OBD_INCOMPAT_XFS_REC) ||
	    !(le32_to_cpu(authoritative->lsd_feature_incompat) & OBD_INCOMPAT_XFS_REC) ||
	    le64_to_cpu(mirror->lsd_last_transno) != 41 ||
	    le64_to_cpu(authoritative->lsd_last_transno) != (logged ? 73 : 41))
		goto corrupt;
	/* These header generations differ only in last_transno by setup. */
	authoritative->lsd_last_transno = mirror->lsd_last_transno;
	if (memcmp(mirror, authoritative, sizeof(*mirror)))
		goto corrupt;
	rc = 0;
	pr_info("PASS preseed: real prepare refused chunk1; MIGRATING and chunk2 absent, both downgrade fences present, independent header generations retained\n");
	goto out;
corrupt:
	rc = -EUCLEAN;
out:
	fput(file);
	return rc;
}

#define RT_BULK_MAGIC 0x6f7364726563626bULL
#define RT_BULK_OWNER "trusted.osd_record_test.bulk"

struct rt_bulk_record {
	__le64 magic;
	__le32 round;
	__le32 page_size;
};

static int rt_bulk_object(struct rt_fixture *f, struct dt_object *obj,
			  bool create)
{
	struct dt_object_format dof = { .dof_type = DFT_REGULAR };
	struct lu_attr attr = { .la_valid = LA_MODE, .la_mode = S_IFREG | 0600 };
	__le64 owner = cpu_to_le64(RT_BULK_MAGIC);
	struct lu_buf buf = { .lb_buf = &owner, .lb_len = sizeof(owner) };
	struct thandle *th;
	int rc;

	th = dt_trans_create(&f->env, f->dev);
	if (IS_ERR(th))
		return PTR_ERR(th);
	th->th_sync = 1;
	if (create) {
		rc = dt_declare_create(&f->env, obj, &attr, NULL, &dof, th);
		if (!rc)
			rc = dt_declare_xattr_set(&f->env, obj, NULL, &buf,
						  RT_BULK_OWNER, LU_XATTR_CREATE, th);
	} else {
		rc = dt_declare_destroy(&f->env, obj, th);
	}
	if (!rc)
		rc = dt_trans_start_local(&f->env, f->dev, th);
	if (!rc) {
		dt_write_lock(&f->env, obj, 0);
		if (create) {
			rc = dt_create(&f->env, obj, &attr, NULL, &dof, th);
			if (!rc)
				rc = dt_xattr_set(&f->env, obj, &buf, RT_BULK_OWNER,
						   LU_XATTR_CREATE, th);
		} else {
			rc = dt_destroy(&f->env, obj, th);
		}
		dt_write_unlock(&f->env, obj);
	}
	return rt_stop(&f->env, f->dev, th, rc);
}

static int rt_bulk_read(struct rt_fixture *f, struct dt_object *obj,
			loff_t offset, unsigned int seed, bool hole)
{
	struct niobuf_remote remote = { .rnb_offset = offset, .rnb_len = PAGE_SIZE };
	struct niobuf_local local = {};
	u8 *expected, *data;
	int rc, count, cleanup;

	/* Demand a fresh file-layer read, not the pages dirtied by commit. */
	rc = invalidate_inode_pages2(osd_dt_obj(obj)->oo_inode->i_mapping);
	if (rc)
		return rc;
	expected = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!expected)
		return -ENOMEM;
	if (hole)
		memset(expected, 0, PAGE_SIZE);
	else
		rt_pattern(expected, PAGE_SIZE, seed);
	count = dt_bufs_get(&f->env, obj, &remote, &local, 1, DT_BUFS_TYPE_READ);
	if (count != 1) {
		rc = count < 0 ? count : -EUCLEAN;
		goto free;
	}
	rc = dt_read_prep(&f->env, obj, &local, count);
	if (!rc) {
		data = kmap_local_page(local.lnb_page);
		if (local.lnb_rc != PAGE_SIZE ||
		    memcmp(data + local.lnb_page_offset, expected, PAGE_SIZE))
			rc = -EUCLEAN;
		kunmap_local(data);
	}
	cleanup = dt_bufs_put(&f->env, obj, &local, count);
	if (!rc)
		rc = cleanup;
free:
	kfree(expected);
	return rc;
}

static int rt_bulk_commit(struct rt_fixture *f, struct dt_object **objects,
			  unsigned int round, bool discontiguous, bool singleton)
{
	struct dt_object *obj[2] = { objects[0], objects[discontiguous ? 0 : 1] };
	struct niobuf_local local[2] = {};
	struct rt_callback result = {};
	struct rt_bulk_record record = {
		.magic = cpu_to_le64(RT_BULK_MAGIC),
		.round = cpu_to_le32(round),
		.page_size = cpu_to_le32(PAGE_SIZE),
	};
	struct lu_buf buf;
	struct thandle *th;
	loff_t pos = RT_BASE;
	size_t record_bytes = singleton ? RT_CHUNK + sizeof(record) : sizeof(record);
	unsigned int i, acquired = 0, count = singleton ? 1 : 2;
	int rc = 0, cleanup;
	ssize_t bytes;
	rt_pattern(f->expected, record_bytes, round);
	memcpy(f->expected, &record, sizeof(record));
	buf = (struct lu_buf) { .lb_buf = f->expected, .lb_len = record_bytes };

	for (i = 0; i < count; i++) {
		struct niobuf_remote remote = {
			.rnb_offset = discontiguous ? (1 + 2 * i) * PAGE_SIZE : 0,
			.rnb_len = PAGE_SIZE,
		};
		u8 *data;

		rc = dt_bufs_get(&f->env, obj[i], &remote, &local[i], 1,
				 DT_BUFS_TYPE_WRITE);
		if (rc != 1) {
			rc = rc < 0 ? rc : -EUCLEAN;
			goto put;
		}
		acquired++;
		rc = dt_write_prep(&f->env, obj[i], &local[i], 1);
		if (rc)
			goto put;
		data = kmap_local_page(local[i].lnb_page);
		rt_pattern(data + local[i].lnb_page_offset, PAGE_SIZE,
			   round * 4 + (discontiguous ? 2 : 0) + i);
		kunmap_local(data);
	}
	th = dt_trans_create(&f->env, f->dev);
	if (IS_ERR(th)) {
		rc = PTR_ERR(th);
		goto put;
	}
	th->th_sync = 1;
	/* Both declarations precede either execution. */
	for (i = 0; i < count && !rc; i++)
		rc = dt_declare_write_commit(&f->env, obj[i], &local[i], 1, th);
	if (!rc)
		rc = dt_declare_write(&f->env, f->obj, &buf, pos, th);
	if (!rc)
		rc = dt_trans_start_local(&f->env, f->dev, th);
	if (!rc) {
		INIT_LIST_HEAD(&result.cb.dcb_linkage);
		result.cb.dcb_func = rt_commit;
		strscpy(result.cb.dcb_name, "osdrec-bulk", sizeof(result.cb.dcb_name));
		rc = dt_trans_cb_add(th, &result.cb);
	}
	for (i = 0; i < count && !rc; i++)
		rc = dt_write_commit(&f->env, obj[i], &local[i], 1, th, 0);
	if (!rc) {
		bytes = dt_write(&f->env, f->obj, &buf, &pos, th);
		if (bytes != record_bytes || pos != RT_BASE + record_bytes)
			rc = bytes < 0 ? bytes : -EIO;
	}
	rc = rt_stop(&f->env, f->dev, th, rc);
	/* Drain even on an error before callback storage/module text disappears. */
	cleanup = dt_sync(&f->env, f->dev);
	if (!rc)
		rc = cleanup;
	if (!rc && (result.calls != 1 || result.error))
		rc = -EUCLEAN;
	for (i = 0; i < count && !rc; i++)
		rc = rt_bulk_read(f, obj[i], local[i].lnb_file_offset,
				  round * 4 + (discontiguous ? 2 : 0) + i, false);
	if (!rc)
		rc = rt_compare(f, record_bytes);
put:
	while (acquired) {
		i = --acquired;
		cleanup = dt_bufs_put(&f->env, obj[i], &local[i], 1);
		if (!rc)
			rc = cleanup;
	}
	return rc;
}

static int rt_bulk(struct rt_fixture *f, bool verify)
{
	struct dt_object *objects[2] = {};
	struct rt_bulk_record record;
	struct lu_buf buf = { .lb_buf = &record, .lb_len = sizeof(record) };
	bool created[2] = {};
	unsigned int i, round = 0;
	loff_t pos = RT_BASE;
	int rc = 0, cleanup;

	for (i = 0; i < 2; i++) {
		struct lu_fid fid;
		__le64 owner = 0;
		struct lu_buf owner_buf = { .lb_buf = &owner, .lb_len = sizeof(owner) };

		lu_local_obj_fid(&fid, 0x7fff0010 + i);
		objects[i] = dt_locate_at(&f->env, f->dev, &fid, &f->top, NULL);
		if (IS_ERR(objects[i])) {
			rc = PTR_ERR(objects[i]);
			objects[i] = NULL;
			goto out;
		}
		if (!verify) {
			if (dt_object_exists(objects[i])) {
				rc = -EEXIST;
				goto out;
			}
			rc = rt_bulk_object(f, objects[i], true);
			created[i] = dt_object_exists(objects[i]);
		} else if (!dt_object_exists(objects[i])) {
			rc = -ENOENT;
		} else {
			rc = dt_xattr_get(&f->env, objects[i], &owner_buf, RT_BULK_OWNER);
			rc = rc == sizeof(owner) && le64_to_cpu(owner) == RT_BULK_MAGIC ?
			     0 : -EUCLEAN;
		}
		if (rc)
			goto out;
	}
	if (!verify) {
		rc = rt_reset(f);
		if (!rc)
			rc = rt_resize(f, RT_BASE + 2 * RT_CHUNK, false);
		for (round = 1; round <= rounds && !rc; round++) {
			rc = rt_bulk_commit(f, objects, round, false, true);
			if (!rc)
				rc = rt_bulk_commit(f, objects, round, false, false);
			if (!rc)
				rc = rt_bulk_commit(f, objects, round, true, false);
		}
		if (!rc)
			pr_info("SETUP bulk: %u repeated two-object and discontiguous commits, callbacks and post-stop data/record reads; reopen before bulk-verify\n", rounds);
	} else {
		rc = dt_record_read(&f->env, f->obj, &buf, &pos);
		if (!rc && (le64_to_cpu(record.magic) != RT_BULK_MAGIC ||
			    le32_to_cpu(record.page_size) != PAGE_SIZE ||
			    !le32_to_cpu(record.round) || le32_to_cpu(record.round) > 4096))
			rc = -EUCLEAN;
		if (!rc) {
			round = le32_to_cpu(record.round);
			rc = rt_bulk_read(f, objects[0], 0, round * 4, false);
		}
		if (!rc)
			rc = rt_bulk_read(f, objects[1], 0, round * 4 + 1, false);
		if (!rc)
			rc = rt_bulk_read(f, objects[0], PAGE_SIZE, round * 4 + 2, false);
		if (!rc)
			rc = rt_bulk_read(f, objects[0], 3 * PAGE_SIZE, round * 4 + 3, false);
		if (!rc)
			rc = rt_bulk_read(f, objects[0], 2 * PAGE_SIZE, 0, true);
		if (!rc) {
			created[0] = created[1] = true;
			pr_info("PASS bulk persistence: both objects, discontiguous ranges, untouched hole and record generation %u after reopen\n", round);
		}
	}
out:
	for (i = 0; i < 2; i++) {
		if (!objects[i])
			continue;
		if (created[i] && (verify || rc)) {
			cleanup = rt_bulk_object(f, objects[i], false);
			if (cleanup)
				pr_err("bulk owned object %u cleanup: %d\n", i, cleanup);
			if (!rc)
				rc = cleanup;
		}
		lu_object_put_nocache(&f->env, &objects[i]->do_lu);
	}
	return rc;
}

static int rt_run(struct rt_fixture *f)
{
	int rc;

	if (!strcmp(mode, "bulk-setup"))
		return rt_bulk(f, false);
	if (!strcmp(mode, "bulk-verify"))
		return rt_bulk(f, true);
	if (!strcmp(mode, "preseed-setup"))
		return rt_preseed_setup(f, false);
	if (!strcmp(mode, "preseed-xattr-setup"))
		return rt_preseed_setup(f, true);
	if (!strcmp(mode, "basic")) {
		rc = rt_concurrent(f);
		if (!rc)
			rc = rt_eof(f);
		if (!rc)
			rc = rt_truncate(f, false);
		if (!rc)
			rc = rt_truncate(f, true);
		return rc;
	}
	if (!strcmp(mode, "growth-setup")) {
		rc = rt_reset(f);
		if (!rc)
			rc = rt_resize(f, RT_BASE, false);
		rt_pattern(f->expected, RT_SLOT, 7);
		if (!rc)
			rc = rt_write(&f->env, f->dev, f->obj, f->expected,
				      RT_SLOT, RT_BASE);
		if (!rc)
			rc = rt_size(f, RT_BASE + RT_SLOT);
		if (!rc)
			rc = rt_compare(f, RT_SLOT);
		if (!rc)
			pr_info("SETUP growth: size=%u record compared; reopen or cut power before growth-verify\n",
				RT_BASE + RT_SLOT);
		return rc;
	}
	if (!strcmp(mode, "growth-verify")) {
		rt_pattern(f->expected, RT_SLOT, 7);
		rc = rt_size(f, RT_BASE + RT_SLOT);
		if (!rc)
			rc = rt_compare(f, RT_SLOT);
		if (!rc)
			pr_info("PASS growth persistence: exact size and complete record after external reopen\n");
		return rc;
	}
	if (!strcmp(mode, "legacy-setup"))
		return rt_upgrade_setup(f, false, false);
	if (!strcmp(mode, "legacy-xattr-setup"))
		return rt_upgrade_setup(f, true, false);
	if (!strcmp(mode, "migration-setup"))
		return rt_upgrade_setup(f, true, true);
	if (!strcmp(mode, "legacy-verify"))
		return rt_upgrade_verify(f, false);
	if (!strcmp(mode, "legacy-xattr-verify") ||
	    !strcmp(mode, "migration-verify"))
		return rt_upgrade_verify(f, true);
	if (!strcmp(mode, "unknown-setup")) {
		struct osd_rec_format format = {
			.version = cpu_to_le32(OSD_REC_FORMAT_VERSION + 1),
			.state = cpu_to_le32(OSD_REC_FORMAT_READY),
		};

		rc = rt_seed(f, RT_CHUNK);
		return rc ? rc : rt_setxattr(f, OSD_REC_FORMAT_XATTR,
					    &format, sizeof(format));
	}
	if (!strcmp(mode, "truncate-setup"))
		return rt_truncate_setup(f);
	if (!strcmp(mode, "truncate-verify"))
		return rt_truncate_verify(f);
	if (!strcmp(mode, "malformed-setup"))
		return rt_malformed_setup(f);
	if (!strcmp(mode, "fault"))
		return rt_fault(f, false);
	if (!strcmp(mode, "stop-fault"))
		return rt_fault(f, true);
	return -EINVAL;
}

static int __init osd_record_test_init(void)
{
	struct dt_object_format dof = { .dof_type = DFT_REGULAR };
	struct lu_attr attr = { .la_valid = LA_MODE, .la_mode = S_IFREG | 0600 };
	struct rt_fixture *f;
	struct obd_device *obd;
	struct lu_fid fid;
	bool verify;
	int rc;

	if (!target || !*target || !rounds || rounds > 4096)
		return -EINVAL;
	if (strcmp(mode, "basic") && strcmp(mode, "growth-setup") &&
	    strcmp(mode, "growth-verify") && strcmp(mode, "truncate-setup") &&
	    strcmp(mode, "truncate-verify") && strcmp(mode, "malformed-setup") &&
	    strcmp(mode, "malformed-verify") && strcmp(mode, "fault") &&
	    strcmp(mode, "stop-fault") && strcmp(mode, "legacy-setup") &&
	    strcmp(mode, "legacy-verify") && strcmp(mode, "legacy-xattr-setup") &&
	    strcmp(mode, "legacy-xattr-verify") && strcmp(mode, "migration-setup") &&
	    strcmp(mode, "migration-verify") && strcmp(mode, "unknown-setup") &&
	    strcmp(mode, "unknown-verify") && strcmp(mode, "last-seq-setup") &&
	    strcmp(mode, "last-seq-verify") && strcmp(mode, "last-seq-invalid") &&
	    strcmp(mode, "config") && strcmp(mode, "preseed-setup") &&
	    strcmp(mode, "preseed-verify") && strcmp(mode, "preseed-xattr-setup") &&
	    strcmp(mode, "preseed-xattr-verify") && strcmp(mode, "bulk-setup") &&
	    strcmp(mode, "bulk-verify"))
		return -EINVAL;
	if (!strcmp(mode, "stop-fault")) {
		if (READ_ONCE(cfs_fail_loc))
			return -EBUSY;
		pr_warn("stop-fault uses global fail_loc: isolated VM, no other targets or fault injection\n");
	}
	obd = class_name2obd(target);
	if (!obd || !obd->obd_type || strcmp(obd->obd_type->typ_name, OSD_XFS_NAME) ||
	    !test_bit(OBDF_ATTACHED, obd->obd_flags) ||
	    !test_bit(OBDF_SET_UP, obd->obd_flags) || !obd->obd_lu_dev ||
	    !obd->obd_lu_dev->ld_site ||
	    obd->obd_lu_dev->ld_site->ls_top_dev != obd->obd_lu_dev) {
		pr_err("refusing target: require a standalone osd-xfs device, not a service stack\n");
		return -ENODEV;
	}
	class_incref(obd, "osd_record_test", THIS_MODULE);
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f) {
		rc = -ENOMEM;
		goto put_obd;
	}
	f->dev = lu2dt_dev(obd->obd_lu_dev);
	rc = lu_env_init(&f->env, LCT_LOCAL | LCT_DT_THREAD);
	if (rc)
		goto free;
	if (!strncmp(mode, "last-seq-", 9)) {
		rc = rt_last_seq(f);
		goto fini_env;
	}
	if (!strcmp(mode, "config")) {
		rc = rt_config(f);
		goto fini_env;
	}
	rc = lu_device_init(&f->top, &rt_type);
	if (rc)
		goto fini_env;
	f->top.ld_ops = &rt_device_ops;
	f->top.ld_site = f->dev->dd_lu_dev.ld_site;
	if (!strcmp(mode, "preseed-verify") ||
	    !strcmp(mode, "preseed-xattr-verify")) {
		rc = rt_preseed_verify(f, !strcmp(mode, "preseed-xattr-verify"));
		goto fini_top;
	}
	/* Reserve a separate non-record local FID for the tracked writeback case. */
	lu_local_obj_fid(&fid, !strcmp(mode, "fault") || !strcmp(mode, "stop-fault") ?
			0x7fff0001 : LAST_RECV_OID);
	verify = strstr(mode, "-verify") != NULL;
	f->obj = dt_locate_at(&f->env, f->dev, &fid, &f->top, NULL);
	if (IS_ERR(f->obj)) {
		rc = PTR_ERR(f->obj);
		if ((!strcmp(mode, "malformed-verify") && rc == -EUCLEAN) ||
		    (!strcmp(mode, "unknown-verify") && rc == -EOPNOTSUPP)) {
			pr_info("PASS %s: initialization refused format before record reads\n",
				mode);
			rc = 0;
		}
		goto fini_top;
	}
	if (!strcmp(mode, "malformed-verify") ||
	    !strcmp(mode, "unknown-verify")) {
		rc = -EUCLEAN;
		goto put_obj;
	}
	if (!dt_object_exists(f->obj)) {
		struct thandle *th;

		if (verify) {
			rc = -ENOENT;
			goto put_obj;
		}
		th = dt_trans_create(&f->env, f->dev);
		if (IS_ERR(th)) {
			rc = PTR_ERR(th);
			goto put_obj;
		}
		th->th_sync = 1;
		rc = dt_declare_create(&f->env, f->obj, &attr, NULL, &dof, th);
		if (!rc)
			rc = dt_trans_start_local(&f->env, f->dev, th);
		if (!rc) {
			dt_write_lock(&f->env, f->obj, 0);
			rc = dt_create(&f->env, f->obj, &attr, NULL, &dof, th);
			dt_write_unlock(&f->env, f->obj);
		}
		rc = rt_stop(&f->env, f->dev, th, rc);
		if (rc)
			goto put_obj;
	}
	pr_info("running mode=%s target=%s fid=" DFID " (destructive scratch fixture)\n",
		mode, target, PFID(&fid));
	rc = rt_run(f);
put_obj:
	/* No cached slice may retain the transient wrapper or module ops. */
	lu_object_put_nocache(&f->env, &f->obj->do_lu);
fini_top:
	f->top.ld_site = NULL;
	lu_device_fini(&f->top);
fini_env:
	lu_env_fini(&f->env);
free:
	kfree(f);
put_obd:
	class_decref(obd, "osd_record_test", THIS_MODULE);
	if (rc)
		pr_err("FAIL mode=%s errno=%d\n", mode, rc);
	return rc;
}

static void __exit osd_record_test_exit(void)
{
}

module_init(osd_record_test_init);
module_exit(osd_record_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Standalone osd-xfs DT record and durability-fence regression probe");
