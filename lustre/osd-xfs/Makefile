# SPDX-License-Identifier: GPL-2.0

#
# This file is part of Lustre, http://www.lustre.org/
#

obj-m += osd_xfs.o
osd_xfs-objs := osd_handler.o osd_object.o osd_oi.o osd_index.o osd_otable.o osd_io.o osd_quota.o osd_scrub.o osd_reserve.o osd_lproc.o

ifdef CONFIG_GCOV_PROFILE_LUSTRE
GCOV_PROFILE := y
endif
