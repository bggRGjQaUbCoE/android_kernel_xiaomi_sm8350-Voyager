// SPDX-License-Identifier: GPL-2.0-only
/* init.c
 *
 * Android Vendor Hook Support
 *
 * Copyright 2020 Google LLC
 */

#include <linux/module.h>
#include <trace/hooks/sched.h>
#include <trace/hooks/topology.h>

#include "sched_priv.h"

extern int pmu_poll_init(void);

static int vh_sched_init(void)
{
	int ret;

	ret = pmu_poll_init();
	if (ret) {
		pr_err("pmu poll init failed\n");
		return ret;
	}

	ret = create_procfs_node();
	if (ret)
		return ret;

        pr_info("pmu init successfully!\n");

	return 0;
}

module_init(vh_sched_init);
MODULE_LICENSE("GPL v2");
