# SPDX-License-Identifier: GPL-2.0
#
# This file is part of Lustre, http://www.lustre.org/
#
# config/lustre-build-xfs.m4
#
# Configure support for the XFS OSD (osd-xfs).
#
# osd-xfs uses the in-kernel XFS through VFS interfaces, plus a small
# kernel API (CONFIG_XFS_OSD, <linux/xfs_osd.h>) that creates an object with
# its identity, and updates its attributes, in single XFS transactions. That
# API is what makes the backend crash consistent, so it is required.
#

#
# LB_XFS_OSD_API
#
# Require <linux/xfs_osd.h>, the exports it declares, and vfs_fiemap_kernel()
# for answering a client's FIEMAP request.
#
AC_DEFUN([LB_XFS_OSD_API], [
xfs_osd_api=no
LB_CHECK_COMPILE([for the XFS single-transaction object API], xfs_osd_api, [
	#include <linux/build_bug.h>
	#include <linux/fiemap.h>
	#include <linux/xfs_osd.h>
], [
	BUILD_BUG_ON(!__same_type(&xfs_osd_create,
		(int (*)(struct inode *, struct dentry *, umode_t,
			 const struct xfs_osd_attr *,
			 const struct xfs_osd_xattr *, unsigned int))0));
	BUILD_BUG_ON(!__same_type(&xfs_osd_setattr,
		(int (*)(struct dentry *, const struct xfs_osd_attr *))0));
	(void)vfs_fiemap_kernel;
], [
	xfs_osd_api=yes
	LB_CHECK_EXPORT([xfs_osd_create], [], [], [xfs_osd_api=no])
	LB_CHECK_EXPORT([xfs_osd_setattr], [], [], [xfs_osd_api=no])
	LB_CHECK_EXPORT([vfs_fiemap_kernel], [], [], [xfs_osd_api=no])
])
]) # LB_XFS_OSD_API

#
# LB_CONFIG_XFS
#
# --enable-xfs / --disable-xfs
#
AC_DEFUN([LB_CONFIG_XFS], [
AC_ARG_ENABLE([xfs],
	AS_HELP_STRING([--disable-xfs],
		       [disable the XFS OSD (osd-xfs)]),
	[], [enable_xfs="maybe"])

AS_IF([test x$enable_server = xno], [
	AS_CASE([$enable_xfs],
		[maybe], [enable_xfs=no],
		[yes], [AC_MSG_ERROR([cannot build osd-xfs when servers are disabled])])
])

AS_IF([test x$enable_modules = xno], [enable_xfs=no])

AS_IF([test x$enable_xfs != xno], [
	dnl osd-xfs mounts the in-kernel xfs filesystem via vfs_kern_mount(),
	dnl so the kernel must actually have XFS available.
	LB_CHECK_CONFIG_IM([XFS_FS], [], [
		AS_CASE([$enable_xfs],
			[maybe], [enable_xfs=no],
			[yes], [AC_MSG_ERROR([

osd-xfs requires CONFIG_XFS_FS to be enabled in the kernel at
$LINUX, either built in or as a module.
])])
	])
])

AS_IF([test x$enable_xfs != xno], [
	LB_XFS_OSD_API
	AS_IF([test x$xfs_osd_api != xyes], [
		AS_CASE([$enable_xfs],
			[maybe], [enable_xfs=no],
			[yes], [AC_MSG_ERROR([

osd-xfs requires a kernel built with CONFIG_XFS_OSD, which provides
<linux/xfs_osd.h> and the xfs_osd_* exports, and vfs_fiemap_kernel().
])])
	])
])

AC_MSG_CHECKING([whether to build osd-xfs])
AS_IF([test x$enable_xfs = xmaybe], [enable_xfs=yes])
AC_MSG_RESULT([$enable_xfs])

AS_IF([test x$enable_xfs = xyes], [
	AC_DEFINE(HAVE_XFS_OSD, 1, [build the XFS OSD])
	AC_SUBST(ENABLE_XFS, yes)
], [
	AC_SUBST(ENABLE_XFS, no)
])

AM_CONDITIONAL([XFS_ENABLED], [test x$enable_xfs = xyes])
]) # LB_CONFIG_XFS
