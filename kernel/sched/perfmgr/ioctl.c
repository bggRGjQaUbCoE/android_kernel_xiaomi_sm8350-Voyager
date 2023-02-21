// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#include "pfmgr_ioctl.h"

#define TAG "PERF_IOCTL"

void (*fpsgo_notify_qudeq_fp)(int qudeq,
		unsigned int startend,
		int pid, unsigned long long identifier);
EXPORT_SYMBOL_GPL(fpsgo_notify_qudeq_fp);
void (*fpsgo_notify_connect_fp)(int pid,
		int connectedAPI, unsigned long long identifier);
EXPORT_SYMBOL_GPL(fpsgo_notify_connect_fp);

struct proc_dir_entry *perfmgr_root;
EXPORT_SYMBOL(perfmgr_root);

static unsigned long perfctl_copy_from_user(void *pvTo,
		const void __user *pvFrom, unsigned long ulBytes)
{
	if (access_ok(pvFrom, ulBytes))
		return __copy_from_user(pvTo, pvFrom, ulBytes);

	return ulBytes;
}

static int device_show(struct seq_file *m, void *v)
{
	return 0;
}

static int device_open(struct inode *inode, struct file *file)
{
	return single_open(file, device_show, inode->i_private);
}

static long device_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	ssize_t ret = 0;
	int pwr_cmd = -1, value1 = -1, value2 = -1, pwr_pid = -1, pwr_fps = -1;
	struct _FPSGO_PACKAGE *msgKM = NULL,
			*msgUM = (struct _FPSGO_PACKAGE *)arg;
	struct _FPSGO_PACKAGE smsgKM;

	msgKM = &smsgKM;

	if (perfctl_copy_from_user(msgKM, msgUM,
				sizeof(struct _FPSGO_PACKAGE))) {
		ret = -EFAULT;
		goto ret_ioctl;
	}

	switch (cmd) {
	case FPSGO_QUEUE:
		if (fpsgo_notify_qudeq_fp)
			fpsgo_notify_qudeq_fp(1,
					msgKM->start, msgKM->tid,
					msgKM->identifier);
		break;
	case FPSGO_DEQUEUE:
		if (fpsgo_notify_qudeq_fp)
			fpsgo_notify_qudeq_fp(0,
					msgKM->start, msgKM->tid,
					msgKM->identifier);
		break;
	case FPSGO_QUEUE_CONNECT:
		if (fpsgo_notify_connect_fp)
			fpsgo_notify_connect_fp(msgKM->tid,
					msgKM->connectedAPI, msgKM->identifier);
		break;
	default:
		pr_debug(TAG "%s %d: unknown cmd %x\n",
			__FILE__, __LINE__, cmd);
		ret = -1;
		goto ret_ioctl;
	}

ret_ioctl:
	return ret;
}

static const struct file_operations Fops = {
	.compat_ioctl = device_ioctl,
	.unlocked_ioctl = device_ioctl,
	.open = device_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void __exit exit_perfctl(void) {}

static int __init init_perfctl(void)
{
	struct proc_dir_entry *pe, *parent;
	int ret_val = 0;


	pr_debug(TAG"Start to init perf_ioctl driver\n");

	parent = proc_mkdir("perfmgr", NULL);
	perfmgr_root = parent;

	pe = proc_create("perf_ioctl", 0664, parent, &Fops);
	if (!pe) {
		pr_debug(TAG"%s failed with %d\n",
				"Creating file node ",
				ret_val);
		ret_val = -ENOMEM;
		goto out_wq;
	}

	pr_debug(TAG"init perf_ioctl driver done\n");

	return 0;

out_wq:
	return ret_val;
}

module_init(init_perfctl);
module_exit(exit_perfctl);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek FPSGO perf_ioctl");
MODULE_AUTHOR("MediaTek Inc.");
