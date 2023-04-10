/* SPDX-License-Identifier: GPL-2.0-only */
#include "stall_util_cal.h"

struct sugov_tunables {
	struct gov_attr_set	attr_set;
	unsigned int		up_rate_limit_us;
	unsigned int		down_rate_limit_us;
	unsigned int		hispeed_load;
	unsigned int		hispeed_freq;
	unsigned int		rtg_boost_freq;
	unsigned int		target_load_thresh;
	unsigned int		target_load_shift;
	bool			pl;

#ifdef CONFIG_OPLUS_UAG_AMU_AWARE
	bool				stall_aware;
	u64				stall_reduce_pct;
	int				report_policy;
#endif

#ifdef CONFIG_OPLUS_FEATURE_SUGOV_TL
	spinlock_t		target_loads_lock;
	unsigned int		*target_loads;
	int			ntarget_loads;
#endif /* CONFIG_OPLUS_FEATURE_SUGOV_TL */
};

struct sugov_policy {
	struct cpufreq_policy	*policy;

	u64 last_ws;
	u64 curr_cycles;
	u64 last_cyc_update_time;
	unsigned long avg_cap;
	struct sugov_tunables	*tunables;
	struct list_head	tunables_hook;
	unsigned long hispeed_util;
	unsigned long rtg_boost_util;
	unsigned long max;
	unsigned int		len;

	raw_spinlock_t		update_lock;	/* For shared policies */
	u64			last_freq_update_time;
	s64			min_rate_limit_ns;
	s64			up_rate_delay_ns;
	s64			down_rate_delay_ns;
	unsigned int		next_freq;
	unsigned int		cached_raw_freq;

	/* The next fields are only needed if fast switch cannot be used: */
	struct			irq_work irq_work;
	struct			kthread_work work;
	struct			mutex work_lock;
	struct			kthread_worker worker;
	struct task_struct	*thread;
	bool			work_in_progress;

	bool			limits_changed;
	bool			need_freq_update;
};

struct sugov_cpu {
	struct update_util_data	update_util;
	struct sugov_policy	*sg_policy;
	unsigned int		cpu;

	bool			iowait_boost_pending;
	unsigned int		iowait_boost;
	u64			last_update;

	struct walt_cpu_load	walt_load;

	unsigned long util;
	unsigned int flags;

	unsigned long		bw_dl;
	unsigned long		max;

	/* The field below is for single-CPU policies only: */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long		saved_idle_calls;
#endif
};

#define MAX_CLUSTERS 3
static int init_flag[MAX_CLUSTERS];

