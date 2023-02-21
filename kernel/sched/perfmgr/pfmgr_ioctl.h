/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __PFMGR_IOCTL_H__
#define __PFMGR_IOCTL_H__
#include <linux/jiffies.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/utsname.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/uaccess.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/notifier.h>
#include <linux/suspend.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/slab.h>

#include <linux/ioctl.h>

extern void (*fpsgo_notify_qudeq_fp)(int qudeq, unsigned int startend,
		int pid, unsigned long long identifier);
extern void (*fpsgo_notify_connect_fp)(int pid, int connectedAPI,
		unsigned long long identifier);

struct _FPSGO_PACKAGE {
	union {
		__u32 tid;
		__s32 fps;
		__s32 cmd;
		__s32 active;
	};
	union {
		__u32 start;
		__u32 connectedAPI;
		__u32 value1;
	};
	union {
		__u64 frame_time;
		__u64 bufID;
		__s64 time_diff;
	};
	__u64 frame_id;
	union {
		__s32 queue_SF;
		__s32 value2;
	};
	__u64 identifier;
};

#define FPSGO_QUEUE                  _IOW('g', 1,  struct _FPSGO_PACKAGE)
#define FPSGO_DEQUEUE                _IOW('g', 3,  struct _FPSGO_PACKAGE)
#define FPSGO_QUEUE_CONNECT          _IOW('g', 15, struct _FPSGO_PACKAGE)
#endif
