// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2021, The Linux Foundation. All rights reserved.
 */
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <../../../drivers/android/binder_internal.h>

#include <trace/events/sched.h>
#include <trace/hooks/sched.h>
#include <trace/hooks/binder.h>

#include "qc_vas.h"

#ifdef CONFIG_SCHED_WALT
/* 1ms default for 20ms window size scaled to 1024 */
unsigned int sysctl_sched_min_task_util_for_boost = 51;
/* 0.68ms default for 20ms window size scaled to 1024 */
unsigned int sysctl_sched_min_task_util_for_colocation = 35;

unsigned int sysctl_em_inflate_pct = 100;
unsigned int sysctl_em_inflate_thres = 1024;

static void create_util_to_cost_pd(struct em_perf_domain *pd)
{
	int util, cpu = cpumask_first(to_cpumask(pd->cpus));
	unsigned long fmax;
	unsigned long scale_cpu;
	struct rq *rq = cpu_rq(cpu);
	struct walt_sched_cluster *cluster = rq->wrq.cluster;

	fmax = (u64)pd->table[pd->nr_cap_states - 1].frequency;
	scale_cpu = arch_scale_cpu_capacity(cpu);

	for (util = 0; util < 1024; util++) {
		int j;

		int f = (fmax * util) / scale_cpu;
		struct em_cap_state *ps = &pd->table[0];

		for (j = 0; j < pd->nr_cap_states; j++) {
			ps = &pd->table[j];
			if (ps->frequency >= f)
				break;
		}
		cluster->util_to_cost[util] = ps->cost;
	}
}

void create_util_to_cost(void)
{
	struct perf_domain *pd;
	struct root_domain *rd = cpu_rq(smp_processor_id())->rd;

	rcu_read_lock();
	pd = rcu_dereference(rd->pd);
	for (; pd; pd = pd->next)
		create_util_to_cost_pd(pd->em_pd);
	rcu_read_unlock();
}

static inline unsigned long
cpu_util_next_walt(int cpu, struct task_struct *p, int dst_cpu)
{
	struct rq *rq = cpu_rq(cpu);
	unsigned long util = rq->wrq.walt_stats.cumulative_runnable_avg_scaled;
	bool queued = task_on_rq_queued(p);

	/*
	 * When task is queued,
	 * (a) The evaluating CPU (cpu) is task's current CPU. If the
	 * task is migrating, discount the task contribution from the
	 * evaluation cpu.
	 * (b) The evaluating CPU (cpu) is task's current CPU. If the
	 * task is NOT migrating, nothing to do. The contribution is
	 * already present on the evaluation CPU.
	 * (c) The evaluating CPU (cpu) is not task's current CPU. But
	 * the task is migrating to the evaluating CPU. So add the
	 * task contribution to it.
	 * (d) The evaluating CPU (cpu) is neither the current CPU nor
	 * the destination CPU. don't care.
	 *
	 * When task is NOT queued i.e waking. Task contribution is not
	 * present on any CPU.
	 *
	 * (a) If the evaluating CPU is the destination CPU, add the task
	 * contribution.
	 * (b) The evaluation CPU is not the destination CPU, don't care.
	 */
	if (unlikely(queued)) {
		if (task_cpu(p) == cpu) {
			if (dst_cpu != cpu)
				util = max_t(long, util - task_util(p), 0);
		} else if (dst_cpu == cpu) {
			util += task_util(p);
		}
	} else if (dst_cpu == cpu) {
		util += task_util(p);
	}

	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

static inline u64
cpu_util_next_walt_prs(int cpu, struct task_struct *p, int dst_cpu, bool prev_dst_same_cluster,
											u64 *prs)
{
	long util = prs[cpu];

	if (p->wts.prev_window) {
		if (!prev_dst_same_cluster) {
			/* intercluster migration of non rtg task - mimic fixups */
			util -= p->wts.prev_window_cpu[cpu];
			if (util < 0)
				util = 0;
			if (cpu == dst_cpu)
				util += p->wts.prev_window;
		}
	} else {
		if (cpu == dst_cpu)
			util += p->wts.demand;
	}

	return util;
}

static inline unsigned long get_util_to_cost(int cpu, unsigned long util)
{
	struct rq *rq = cpu_rq(cpu);

	if (cpu == 0 && util > sysctl_em_inflate_thres)
		return mult_frac(rq->wrq.cluster->util_to_cost[util], sysctl_em_inflate_pct, 100);
	else
		return rq->wrq.cluster->util_to_cost[util];
}

/**
 * walt_em_cpu_energy() - Estimates the energy consumed by the CPUs of a
		performance domain
 * @pd		: performance domain for which energy has to be estimated
 * @max_util	: highest utilization among CPUs of the domain
 * @sum_util	: sum of the utilization of all CPUs in the domain
 *
 * This function must be used only for CPU devices. There is no validation,
 * i.e. if the EM is a CPU type and has cpumask allocated. It is called from
 * the scheduler code quite frequently and that is why there is not checks.
 *
 * Return: the sum of the energy consumed by the CPUs of the domain assuming
 * a capacity state satisfying the max utilization of the domain.
 */
static inline unsigned long walt_em_cpu_energy(struct em_perf_domain *pd,
				unsigned long max_util, unsigned long sum_util,
				struct compute_energy_output *output, unsigned int x)
{
	unsigned long scale_cpu, cost;
	int cpu;

#if defined(CONFIG_OPLUS_FEATURE_SUGOV_TL) || defined(CONFIG_OPLUS_UAG_USE_TL)
	struct cpufreq_policy policy;
	unsigned long raw_util = max_util;
#endif

	if (!sum_util)
		return 0;

	/*
	 * In order to predict the capacity state, map the utilization of the
	 * most utilized CPU of the performance domain to a requested frequency,
	 * like schedutil.
	 */
	cpu = cpumask_first(to_cpumask(pd->cpus));
	scale_cpu = arch_scale_cpu_capacity(cpu);

#if defined(CONFIG_OPLUS_FEATURE_SUGOV_TL) || defined(CONFIG_OPLUS_UAG_USE_TL)
	if (!cpufreq_get_policy(&policy, cpu))
		trace_android_vh_map_util_freq_new(max_util, max_util, max_util, &max_util, &policy, NULL);

	if (max_util == raw_util)
		max_util = max_util + (max_util >> 2); /* account  for TARGET_LOAD usually 80 */
#else /* !CONFIG_OPLUS_FEATURE_SUGOV_TL */
	max_util = max_util + (max_util >> 2); /* account  for TARGET_LOAD usually 80 */
#endif /* CONFIG_OPLUS_FEATURE_SUGOV_TL */
	max_util = max(max_util,
			(arch_scale_freq_capacity(cpu) * scale_cpu) >>
			SCHED_CAPACITY_SHIFT);

	/*
	 * The capacity of a CPU in the domain at the performance state (ps)
	 * can be computed as:
	 *
	 *             ps->freq * scale_cpu
	 *   ps->cap = --------------------                          (1)
	 *                 cpu_max_freq
	 *
	 * So, ignoring the costs of idle states (which are not available in
	 * the EM), the energy consumed by this CPU at that performance state
	 * is estimated as:
	 *
	 *             ps->power * cpu_util
	 *   cpu_nrg = --------------------                          (2)
	 *                   ps->cap
	 *
	 * since 'cpu_util / ps->cap' represents its percentage of busy time.
	 *
	 *   NOTE: Although the result of this computation actually is in
	 *         units of power, it can be manipulated as an energy value
	 *         over a scheduling period, since it is assumed to be
	 *         constant during that interval.
	 *
	 * By injecting (1) in (2), 'cpu_nrg' can be re-expressed as a product
	 * of two terms:
	 *
	 *             ps->power * cpu_max_freq   cpu_util
	 *   cpu_nrg = ------------------------ * ---------          (3)
	 *                    ps->freq            scale_cpu
	 *
	 * The first term is static, and is stored in the em_cap_state struct
	 * as 'ps->cost'.
	 *
	 * Since all CPUs of the domain have the same micro-architecture, they
	 * share the same 'ps->cost', and the same CPU capacity. Hence, the
	 * total energy of the domain (which is the simple sum of the energy of
	 * all of its CPUs) can be factorized as:
	 *
	 *            ps->cost * \Sum cpu_util
	 *   pd_nrg = ------------------------                       (4)
	 *                  scale_cpu
	 */
	if (max_util >= 1024)
		max_util = 1023;

	cost = get_util_to_cost(cpu, max_util);

	if (output) {
		output->cost[x] = cost;
		output->max_util[x] = max_util;
		output->sum_util[x] = sum_util;
	}
	return cost * sum_util / scale_cpu;
}

/*
 * walt_pd_compute_energy(): Estimates the energy that @pd would consume if @p was
 * migrated to @dst_cpu. compute_energy() predicts what will be the utilization
 * landscape of @pd's CPUs after the task migration, and uses the Energy Model
 * to compute what would be the energy if we decided to actually migrate that
 * task.
 */
static long
walt_pd_compute_energy(struct task_struct *p, int dst_cpu, struct perf_domain *pd, u64 *prs,
		struct compute_energy_output *output, unsigned int x)
{
	struct cpumask *pd_mask = perf_domain_span(pd);
	unsigned long max_util = 0, sum_util = 0;
	int cpu;
	unsigned long cpu_util;
	bool prev_dst_same_cluster = false;

	if (same_cluster(task_cpu(p), dst_cpu))
		prev_dst_same_cluster = true;

	/*
	 * The capacity state of CPUs of the current rd can be driven by CPUs
	 * of another rd if they belong to the same pd. So, account for the
	 * utilization of these CPUs too by masking pd with cpu_online_mask
	 * instead of the rd span.
	 *
	 * If an entire pd is outside of the current rd, it will not appear in
	 * its pd list and will not be accounted by compute_energy().
	 */
	for_each_cpu_and(cpu, pd_mask, cpu_online_mask) {
		sum_util += cpu_util_next_walt(cpu, p, dst_cpu);
		cpu_util = cpu_util_next_walt_prs(cpu, p, dst_cpu, prev_dst_same_cluster, prs);
		max_util = max(max_util, cpu_util);
	}

	max_util = scale_time_to_util(max_util);

	if (output)
		output->cluster_first_cpu[x] = cpumask_first(pd_mask);

	return walt_em_cpu_energy(pd->em_pd, max_util, sum_util, output, x);
}

inline long
walt_compute_energy(struct task_struct *p, int dst_cpu, struct perf_domain *pd,
			cpumask_t *candidates, u64 *prs, struct compute_energy_output *output)
{
	long energy = 0;
	unsigned int x = 0;

	for (; pd; pd = pd->next) {
		struct cpumask *pd_mask = perf_domain_span(pd);

		if (cpumask_intersects(candidates, pd_mask)
				|| cpumask_test_cpu(task_cpu(p), pd_mask)) {
			energy += walt_pd_compute_energy(p, dst_cpu, pd, prs, output, x);
			x++;
		}
	}

	return energy;
}

static int
kick_active_balance(struct rq *rq, struct task_struct *p, int new_cpu)
{
	unsigned long flags;
	int rc = 0;

	/* Invoke active balance to force migrate currently running task */
	raw_spin_lock_irqsave(&rq->lock, flags);
	if (!rq->active_balance) {
		rq->active_balance = 1;
		rq->push_cpu = new_cpu;
		get_task_struct(p);
		rq->wrq.push_task = p;
		rc = 1;
	}
	raw_spin_unlock_irqrestore(&rq->lock, flags);

	return rc;
}

struct walt_rotate_work {
	struct work_struct w;
	struct task_struct *src_task;
	struct task_struct *dst_task;
	int src_cpu;
	int dst_cpu;
};

static DEFINE_PER_CPU(struct walt_rotate_work, walt_rotate_works);

static void walt_rotate_work_func(struct work_struct *work)
{
	struct walt_rotate_work *wr = container_of(work,
					struct walt_rotate_work, w);
	struct rq *src_rq = cpu_rq(wr->src_cpu), *dst_rq = cpu_rq(wr->dst_cpu);
	unsigned long flags;

	migrate_swap(wr->src_task, wr->dst_task, wr->dst_cpu, wr->src_cpu);

	put_task_struct(wr->src_task);
	put_task_struct(wr->dst_task);

	local_irq_save(flags);
	double_rq_lock(src_rq, dst_rq);

	dst_rq->active_balance = 0;
	src_rq->active_balance = 0;

	double_rq_unlock(src_rq, dst_rq);
	local_irq_restore(flags);

	clear_reserved(wr->src_cpu);
	clear_reserved(wr->dst_cpu);
}

void walt_rotate_work_init(void)
{
	int i;

	for_each_possible_cpu(i) {
		struct walt_rotate_work *wr = &per_cpu(walt_rotate_works, i);

		INIT_WORK(&wr->w, walt_rotate_work_func);
	}
}

#define WALT_ROTATION_THRESHOLD_NS      16000000
static void walt_check_for_rotation(struct rq *src_rq)
{
	u64 wc, wait, max_wait = 0, run, max_run = 0;
	int deserved_cpu = nr_cpu_ids, dst_cpu = nr_cpu_ids;
	int i, src_cpu = cpu_of(src_rq);
	struct rq *dst_rq;
	struct walt_rotate_work *wr = NULL;

	if (!walt_rotation_enabled)
		return;

	if (!is_min_capacity_cpu(src_cpu))
		return;

	wc = sched_ktime_clock();
	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);

		if (!is_min_capacity_cpu(i))
			continue;

		if (is_reserved(i))
			continue;

		if (!rq->misfit_task_load || rq->curr->sched_class !=
							&fair_sched_class)
			continue;

		wait = wc - rq->curr->wts.last_enqueued_ts;
		if (wait > max_wait) {
			max_wait = wait;
			deserved_cpu = i;
		}
	}

	if (deserved_cpu != src_cpu)
		return;

	for_each_possible_cpu(i) {
		struct rq *rq = cpu_rq(i);

		if (is_min_capacity_cpu(i))
			continue;

		if (is_reserved(i))
			continue;

		if (rq->curr->sched_class != &fair_sched_class)
			continue;

		if (rq->nr_running > 1)
			continue;

		run = wc - rq->curr->wts.last_enqueued_ts;

		if (run < WALT_ROTATION_THRESHOLD_NS)
			continue;

		if (run > max_run) {
			max_run = run;
			dst_cpu = i;
		}
	}

	if (dst_cpu == nr_cpu_ids)
		return;

	dst_rq = cpu_rq(dst_cpu);

	double_rq_lock(src_rq, dst_rq);
	if (dst_rq->curr->sched_class == &fair_sched_class &&
		!src_rq->active_balance && !dst_rq->active_balance) {
		get_task_struct(src_rq->curr);
		get_task_struct(dst_rq->curr);

		mark_reserved(src_cpu);
		mark_reserved(dst_cpu);
		wr = &per_cpu(walt_rotate_works, src_cpu);

		wr->src_task = src_rq->curr;
		wr->dst_task = dst_rq->curr;

		wr->src_cpu = src_cpu;
		wr->dst_cpu = dst_cpu;
		dst_rq->active_balance = 1;
		src_rq->active_balance = 1;
	}

	double_rq_unlock(src_rq, dst_rq);

	if (wr)
		queue_work_on(src_cpu, system_highpri_wq, &wr->w);
}

static DEFINE_RAW_SPINLOCK(migration_lock);
void check_for_migration(struct rq *rq, struct task_struct *p)
{
	int active_balance;
	int new_cpu = -1;
	int prev_cpu = task_cpu(p);
	int ret;

	if (rq->misfit_task_load) {
		if (rq->curr->state != TASK_RUNNING ||
		    rq->curr->nr_cpus_allowed == 1)
			return;

		if (walt_rotation_enabled) {
			raw_spin_lock(&migration_lock);
			walt_check_for_rotation(rq);
			raw_spin_unlock(&migration_lock);
			return;
		}

		raw_spin_lock(&migration_lock);
		rcu_read_lock();
		new_cpu = find_energy_efficient_cpu(p, prev_cpu, 0, 1);
		rcu_read_unlock();
		if ((new_cpu >= 0) && (new_cpu != prev_cpu) &&
		    (capacity_orig_of(new_cpu) > capacity_orig_of(prev_cpu))) {
			active_balance = kick_active_balance(rq, p, new_cpu);
			if (active_balance) {
				mark_reserved(new_cpu);
				raw_spin_unlock(&migration_lock);
				ret = stop_one_cpu_nowait(prev_cpu,
					active_load_balance_cpu_stop, rq,
					&rq->active_balance_work);
				if (!ret)
					clear_reserved(new_cpu);
				else
					wake_up_if_idle(new_cpu);
				return;
			}
		}
		raw_spin_unlock(&migration_lock);
	}
}

int sched_init_task_load_show(struct seq_file *m, void *v)
{
	struct inode *inode = m->private;
	struct task_struct *p;

	p = get_proc_task(inode);
	if (!p)
		return -ESRCH;

	seq_printf(m, "%d\n", sched_get_init_task_load(p));

	put_task_struct(p);

	return 0;
}

ssize_t
sched_init_task_load_write(struct file *file, const char __user *buf,
	    size_t count, loff_t *offset)
{
	struct inode *inode = file_inode(file);
	struct task_struct *p;
	char buffer[PROC_NUMBUF];
	int init_task_load, err;

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count)) {
		err = -EFAULT;
		goto out;
	}

	err = kstrtoint(strstrip(buffer), 0, &init_task_load);
	if (err)
		goto out;

	p = get_proc_task(inode);
	if (!p)
		return -ESRCH;

	err = sched_set_init_task_load(p, init_task_load);

	put_task_struct(p);

out:
	return err < 0 ? err : count;
}

int sched_init_task_load_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_init_task_load_show, inode);
}

int sched_group_id_show(struct seq_file *m, void *v)
{
	struct inode *inode = m->private;
	struct task_struct *p;

	p = get_proc_task(inode);
	if (!p)
		return -ESRCH;

	seq_printf(m, "%d\n", sched_get_group_id(p));

	put_task_struct(p);

	return 0;
}

ssize_t
sched_group_id_write(struct file *file, const char __user *buf,
	    size_t count, loff_t *offset)
{
	struct inode *inode = file_inode(file);
	struct task_struct *p;
	char buffer[PROC_NUMBUF];
	int group_id, err;

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count)) {
		err = -EFAULT;
		goto out;
	}

	err = kstrtoint(strstrip(buffer), 0, &group_id);
	if (err)
		goto out;

	p = get_proc_task(inode);
	if (!p)
		return -ESRCH;

	err = sched_set_group_id(p, group_id);

	put_task_struct(p);

out:
	return err < 0 ? err : count;
}

int sched_group_id_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_group_id_show, inode);
}

#ifdef CONFIG_SMP
/*
 * Print out various scheduling related per-task fields:
 */
int sched_wake_up_idle_show(struct seq_file *m, void *v)
{
	struct inode *inode = m->private;
	struct task_struct *p;

	p = get_proc_task(inode);
	if (!p)
		return -ESRCH;

	seq_printf(m, "%d\n", sched_get_wake_up_idle(p));

	put_task_struct(p);

	return 0;
}

ssize_t
sched_wake_up_idle_write(struct file *file, const char __user *buf,
	    size_t count, loff_t *offset)
{
	struct inode *inode = file_inode(file);
	struct task_struct *p;
	char buffer[PROC_NUMBUF];
	int wake_up_idle, err;

	memset(buffer, 0, sizeof(buffer));
	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;
	if (copy_from_user(buffer, buf, count)) {
		err = -EFAULT;
		goto out;
	}

	err = kstrtoint(strstrip(buffer), 0, &wake_up_idle);
	if (err)
		goto out;

	p = get_proc_task(inode);
	if (!p)
		return -ESRCH;

	err = sched_set_wake_up_idle(p, wake_up_idle);

	put_task_struct(p);

out:
	return err < 0 ? err : count;
}

int sched_wake_up_idle_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_wake_up_idle_show, inode);
}

int group_balance_cpu_not_isolated(struct sched_group *sg)
{
	cpumask_t cpus;

	cpumask_and(&cpus, sched_group_span(sg), group_balance_mask(sg));
	cpumask_andnot(&cpus, &cpus, cpu_isolated_mask);
	return cpumask_first(&cpus);
}
#endif /* CONFIG_SMP */

#ifdef CONFIG_PROC_SYSCTL
static void sched_update_updown_migrate_values(bool up)
{
	int i = 0, cpu;
	struct walt_sched_cluster *cluster;
	int cap_margin_levels = num_sched_clusters - 1;

	if (cap_margin_levels > 1) {
		/*
		 * No need to worry about CPUs in last cluster
		 * if there are more than 2 clusters in the system
		 */
		for_each_sched_cluster(cluster) {
			for_each_cpu(cpu, &cluster->cpus) {
				if (up)
					sched_capacity_margin_up[cpu] =
					sysctl_sched_capacity_margin_up[i];
				else
					sched_capacity_margin_down[cpu] =
					sysctl_sched_capacity_margin_down[i];
			}

			if (++i >= cap_margin_levels)
				break;
		}
	} else {
		for_each_possible_cpu(cpu) {
			if (up)
				sched_capacity_margin_up[cpu] =
				sysctl_sched_capacity_margin_up[0];
			else
				sched_capacity_margin_down[cpu] =
				sysctl_sched_capacity_margin_down[0];
		}
	}
}

int sched_updown_migrate_handler(struct ctl_table *table, int write,
				void __user *buffer, size_t *lenp,
				loff_t *ppos)
{
	int ret, i;
	unsigned int *data = (unsigned int *)table->data;
	unsigned int *old_val;
	static DEFINE_MUTEX(mutex);
	int cap_margin_levels = num_sched_clusters ? num_sched_clusters - 1 : 0;

	if (cap_margin_levels <= 0)
		return -EINVAL;

	mutex_lock(&mutex);

	if (table->maxlen != (sizeof(unsigned int) * cap_margin_levels))
		table->maxlen = sizeof(unsigned int) * cap_margin_levels;

	if (!write) {
		ret = proc_douintvec_capacity(table, write, buffer, lenp, ppos);
		goto unlock_mutex;
	}

	/*
	 * Cache the old values so that they can be restored
	 * if either the write fails (for example out of range values)
	 * or the downmigrate and upmigrate are not in sync.
	 */
	old_val = kzalloc(table->maxlen, GFP_KERNEL);
	if (!old_val) {
		ret = -ENOMEM;
		goto unlock_mutex;
	}

	memcpy(old_val, data, table->maxlen);

	ret = proc_douintvec_capacity(table, write, buffer, lenp, ppos);

	if (ret) {
		memcpy(data, old_val, table->maxlen);
		goto free_old_val;
	}

	for (i = 0; i < cap_margin_levels; i++) {
		if (sysctl_sched_capacity_margin_up[i] >
				sysctl_sched_capacity_margin_down[i]) {
			memcpy(data, old_val, table->maxlen);
			ret = -EINVAL;
			goto free_old_val;
		}
	}

	sched_update_updown_migrate_values(data ==
					&sysctl_sched_capacity_margin_up[0]);

free_old_val:
	kfree(old_val);
unlock_mutex:
	mutex_unlock(&mutex);

	return ret;
}
#endif /* CONFIG_PROC_SYSCTL */

int sched_isolate_count(const cpumask_t *mask, bool include_offline)
{
	cpumask_t count_mask = CPU_MASK_NONE;

	if (include_offline) {
		cpumask_complement(&count_mask, cpu_online_mask);
		cpumask_or(&count_mask, &count_mask, cpu_isolated_mask);
		cpumask_and(&count_mask, &count_mask, mask);
	} else {
		cpumask_and(&count_mask, mask, cpu_isolated_mask);
	}

	return cpumask_weight(&count_mask);
}

#ifdef CONFIG_HOTPLUG_CPU
static int do_isolation_work_cpu_stop(void *data)
{
	unsigned int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct rq_flags rf;

	local_irq_disable();

	irq_migrate_all_off_this_cpu();

	sched_ttwu_pending();

	/* Update our root-domain */
	rq_lock(rq, &rf);

	/*
	 * Temporarily mark the rq as offline. This will allow us to
	 * move tasks off the CPU.
	 */
	if (rq->rd) {
		BUG_ON(!cpumask_test_cpu(cpu, rq->rd->span));
		set_rq_offline(rq);
	}

	migrate_tasks(rq, &rf, false);

	if (rq->rd)
		set_rq_online(rq);
	rq_unlock(rq, &rf);

	clear_walt_request(cpu);
	local_irq_enable();
	return 0;
}

static int do_unisolation_work_cpu_stop(void *data)
{
	watchdog_enable(smp_processor_id());
	return 0;
}

static void sched_update_group_capacities(int cpu)
{
	struct sched_domain *sd;

	mutex_lock(&sched_domains_mutex);
	rcu_read_lock();

	for_each_domain(cpu, sd) {
		int balance_cpu = group_balance_cpu(sd->groups);

		init_sched_groups_capacity(cpu, sd);
		/*
		 * Need to ensure this is also called with balancing
		 * cpu.
		 */
		if (cpu != balance_cpu)
			init_sched_groups_capacity(balance_cpu, sd);
	}

	rcu_read_unlock();
	mutex_unlock(&sched_domains_mutex);
}

static unsigned int cpu_isolation_vote[NR_CPUS];

/*
 * 1) CPU is isolated and cpu is offlined:
 *	Unisolate the core.
 * 2) CPU is not isolated and CPU is offlined:
 *	No action taken.
 * 3) CPU is offline and request to isolate
 *	Request ignored.
 * 4) CPU is offline and isolated:
 *	Not a possible state.
 * 5) CPU is online and request to isolate
 *	Normal case: Isolate the CPU
 * 6) CPU is not isolated and comes back online
 *	Nothing to do
 *
 * Note: The client calling sched_isolate_cpu() is repsonsible for ONLY
 * calling sched_unisolate_cpu() on a CPU that the client previously isolated.
 * Client is also responsible for unisolating when a core goes offline
 * (after CPU is marked offline).
 */
int sched_isolate_cpu(int cpu)
{
	struct rq *rq;
	cpumask_t avail_cpus;
	int ret_code = 0;
	u64 start_time = 0;

	if (trace_sched_isolate_enabled())
		start_time = sched_clock();

	cpu_maps_update_begin();

	cpumask_andnot(&avail_cpus, cpu_online_mask, cpu_isolated_mask);

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_possible(cpu) ||
				!cpu_online(cpu) || cpu >= NR_CPUS) {
		ret_code = -EINVAL;
		goto out;
	}

	rq = cpu_rq(cpu);

	if (++cpu_isolation_vote[cpu] > 1)
		goto out;

	/* We cannot isolate ALL cpus in the system */
	if (cpumask_weight(&avail_cpus) == 1) {
		--cpu_isolation_vote[cpu];
		ret_code = -EINVAL;
		goto out;
	}

	/*
	 * There is a race between watchdog being enabled by hotplug and
	 * core isolation disabling the watchdog. When a CPU is hotplugged in
	 * and the hotplug lock has been released the watchdog thread might
	 * not have run yet to enable the watchdog.
	 * We have to wait for the watchdog to be enabled before proceeding.
	 */
	if (!watchdog_configured(cpu)) {
		msleep(20);
		if (!watchdog_configured(cpu)) {
			--cpu_isolation_vote[cpu];
			ret_code = -EBUSY;
			goto out;
		}
	}

	set_cpu_isolated(cpu, true);
	cpumask_clear_cpu(cpu, &avail_cpus);

	/* Migrate timers */
	smp_call_function_any(&avail_cpus, hrtimer_quiesce_cpu, &cpu, 1);
	smp_call_function_any(&avail_cpus, timer_quiesce_cpu, &cpu, 1);

	watchdog_disable(cpu);
	irq_lock_sparse();
	stop_cpus(cpumask_of(cpu), do_isolation_work_cpu_stop, NULL);
	irq_unlock_sparse();

	calc_load_migrate(rq);
	update_max_interval();
	sched_update_group_capacities(cpu);

out:
	cpu_maps_update_done();
	trace_sched_isolate(cpu, cpumask_bits(cpu_isolated_mask)[0],
			    start_time, 1);
	return ret_code;
}

/*
 * Note: The client calling sched_isolate_cpu() is repsonsible for ONLY
 * calling sched_unisolate_cpu() on a CPU that the client previously isolated.
 * Client is also responsible for unisolating when a core goes offline
 * (after CPU is marked offline).
 */
int sched_unisolate_cpu_unlocked(int cpu)
{
	int ret_code = 0;
	u64 start_time = 0;

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_possible(cpu)
						|| cpu >= NR_CPUS) {
		ret_code = -EINVAL;
		goto out;
	}

	if (trace_sched_isolate_enabled())
		start_time = sched_clock();

	if (!cpu_isolation_vote[cpu]) {
		ret_code = -EINVAL;
		goto out;
	}

	if (--cpu_isolation_vote[cpu])
		goto out;

	set_cpu_isolated(cpu, false);
	update_max_interval();
	sched_update_group_capacities(cpu);

	if (cpu_online(cpu)) {
		stop_cpus(cpumask_of(cpu), do_unisolation_work_cpu_stop, NULL);

		/* Kick CPU to immediately do load balancing */
		if (!atomic_fetch_or(NOHZ_KICK_MASK, nohz_flags(cpu)))
			smp_send_reschedule(cpu);
	}

out:
	trace_sched_isolate(cpu, cpumask_bits(cpu_isolated_mask)[0],
			    start_time, 0);
	return ret_code;
}

int sched_unisolate_cpu(int cpu)
{
	int ret_code;

	cpu_maps_update_begin();
	ret_code = sched_unisolate_cpu_unlocked(cpu);
	cpu_maps_update_done();
	return ret_code;
}

/*
 * Remove a task from the runqueue and pretend that it's migrating. This
 * should prevent migrations for the detached task and disallow further
 * changes to tsk_cpus_allowed.
 */
void
detach_one_task_core(struct task_struct *p, struct rq *rq,
						struct list_head *tasks)
{
	lockdep_assert_held(&rq->lock);

	p->on_rq = TASK_ON_RQ_MIGRATING;
	deactivate_task(rq, p, 0);
	list_add(&p->se.group_node, tasks);
}

void attach_tasks_core(struct list_head *tasks, struct rq *rq)
{
	struct task_struct *p;

	lockdep_assert_held(&rq->lock);

	while (!list_empty(tasks)) {
		p = list_first_entry(tasks, struct task_struct, se.group_node);
		list_del_init(&p->se.group_node);

		BUG_ON(task_rq(p) != rq);
		activate_task(rq, p, 0);
		p->on_rq = TASK_ON_RQ_QUEUED;
	}
}
#endif /* CONFIG_HOTPLUG_CPU */
/*
 * Higher prio mvp can preempt lower prio mvp.
 *
 * However, the lower prio MVP slice will be more since we expect them to
 * be the work horses. For example, binders will have higher prio MVP and
 * they can preempt long running rtg prio tasks but binders loose their
 * powers with in 3 msec where as rtg prio tasks can run more than that.
 */
int walt_get_mvp_task_prio(struct task_struct *p)
{
#if (!IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST))
	if (walt_procfs_low_latency_task(p) ||
			walt_pipeline_low_latency_task(p))
		return WALT_LL_PIPE_MVP;

	if (per_task_boost(p) == TASK_BOOST_STRICT_MAX)
		return WALT_TASK_BOOST_MVP;

	if (walt_binder_low_latency_task(p))
		return WALT_BINDER_MVP;

	if (task_rtg_high_prio(p))
		return WALT_RTG_MVP;
#endif

	return WALT_NOT_MVP;
}

static inline unsigned int walt_cfs_mvp_task_limit(struct task_struct *p)
{
	/* Binder MVP tasks are high prio but have only single slice */
	if (p->wts.mvp_prio == WALT_BINDER_MVP)
		return WALT_MVP_SLICE;

	return WALT_MVP_LIMIT;
}

static void walt_cfs_insert_mvp_task(struct rq *rq, struct task_struct *p,
				     bool at_front)
{
	struct list_head *pos;

	list_for_each(pos, &rq->wrq.mvp_tasks) {
		struct walt_task_struct *tmp_p = container_of(pos, struct walt_task_struct,
								mvp_list);

		if (at_front) {
			if (p->wts.mvp_prio >= tmp_p->mvp_prio)
				break;
		} else {
			if (p->wts.mvp_prio > tmp_p->mvp_prio)
				break;
		}
	}

	list_add(&p->wts.mvp_list, pos->prev);
	rq->wrq.num_mvp_tasks++;
}

void walt_cfs_deactivate_mvp_task(struct rq *rq, struct task_struct *p)
{
	list_del_init(&p->wts.mvp_list);
	p->wts.mvp_prio = WALT_NOT_MVP;
	rq->wrq.num_mvp_tasks--;
}

/*
 * MVP task runtime update happens here. Three possibilities:
 *
 * de-activated: The MVP consumed its runtime. Non MVP can preempt.
 * slice expired: MVP slice is expired and other MVP can preempt.
 * slice not expired: This MVP task can continue to run.
 */
static void walt_cfs_account_mvp_runtime(struct rq *rq, struct task_struct *curr)
{
	u64 slice;
	unsigned int limit;

	lockdep_assert_held(&rq->lock);

	/*
	 * RQ clock update happens in tick path in the scheduler.
	 * Since we drop the lock in the scheduler before calling
	 * into vendor hook, it is possible that update flags are
	 * reset by another rq lock and unlock. Do the update here
	 * if required.
	 */
	if (!(rq->clock_update_flags & RQCF_UPDATED))
		update_rq_clock(rq);

	if (curr->se.sum_exec_runtime > curr->wts.sum_exec_snapshot_for_total)
		curr->wts.total_exec = curr->se.sum_exec_runtime - curr->wts.sum_exec_snapshot_for_total;
	else
		curr->wts.total_exec = 0;

	if (curr->se.sum_exec_runtime > curr->wts.sum_exec_snapshot_for_slice)
		slice = curr->se.sum_exec_runtime - curr->wts.sum_exec_snapshot_for_slice;
	else
		slice = 0;

	/* slice is not expired */
	if (slice < WALT_MVP_SLICE)
		return;

	curr->wts.sum_exec_snapshot_for_slice = curr->se.sum_exec_runtime;
	/*
	 * slice is expired, check if we have to deactivate the
	 * MVP task, otherwise requeue the task in the list so
	 * that other MVP tasks gets a chance.
	 */

	limit = walt_cfs_mvp_task_limit(curr);
	if (curr->wts.total_exec > limit) {
		walt_cfs_deactivate_mvp_task(rq, curr);
		trace_walt_cfs_deactivate_mvp_task(curr, &curr->wts, limit);
		return;
	}

	if (rq->wrq.num_mvp_tasks == 1)
		return;

	/* slice expired. re-queue the task */
	list_del(&curr->wts.mvp_list);
	rq->wrq.num_mvp_tasks--;
	walt_cfs_insert_mvp_task(rq, curr, false);
}

void walt_cfs_enqueue_task(struct rq *rq, struct task_struct *p)
{
	int mvp_prio = walt_get_mvp_task_prio(p);

	lockdep_assert_held(&rq->lock);

	if (mvp_prio == WALT_NOT_MVP)
		return;

	/*
	 * This can happen during migration or enq/deq for prio/class change.
	 * it was once MVP but got demoted, it will not be MVP until
	 * it goes to sleep again.
	 */
	if (p->wts.total_exec > walt_cfs_mvp_task_limit(p))
		return;

	p->wts.mvp_prio = mvp_prio;
	walt_cfs_insert_mvp_task(rq, p, task_running(rq, p));

	/*
	 * We inserted the task at the appropriate position. Take the
	 * task runtime snapshot. From now onwards we use this point as a
	 * baseline to enforce the slice and demotion.
	 */
	if (!p->wts.total_exec) /* queue after sleep */ {
		p->wts.sum_exec_snapshot_for_total = p->se.sum_exec_runtime;
		p->wts.sum_exec_snapshot_for_slice = p->se.sum_exec_runtime;
	}
}

void walt_cfs_dequeue_task(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_held(&rq->lock);

	if (!list_empty(&p->wts.mvp_list) && p->wts.mvp_list.next)
		walt_cfs_deactivate_mvp_task(rq, p);

	/*
	 * Reset the exec time during sleep so that it starts
	 * from scratch upon next wakeup. total_exec should
	 * be preserved when task is enq/deq while it is on
	 * runqueue.
	 */
	if (READ_ONCE(p->state) != TASK_RUNNING)
		p->wts.total_exec = 0;
}

/*
 * When preempt = false and nopreempt = false, we leave the preemption
 * decision to CFS.
 */
static void walt_cfs_check_preempt_wakeup(void *unused, struct rq *rq, struct task_struct *p,
					  bool *preempt, bool *nopreempt, int wake_flags,
					  struct sched_entity *se, struct sched_entity *pse,
					  int next_buddy_marked, unsigned int granularity)
{
	struct task_struct *wts_p = p;
	struct task_struct *c = rq->curr;
	struct task_struct *wts_c = rq->curr;
	bool resched = false;
	bool p_is_mvp, curr_is_mvp;

	if (unlikely(walt_disabled))
		return;

	p_is_mvp = !list_empty(&wts_p->wts.mvp_list) && wts_p->wts.mvp_list.next;
	curr_is_mvp = !list_empty(&wts_c->wts.mvp_list) && wts_c->wts.mvp_list.next;

	/*
	 * current is not MVP, so preemption decision
	 * is simple.
	 */
	if (!curr_is_mvp) {
		if (p_is_mvp)
			goto preempt;
		return; /* CFS decides preemption */
	}

	/*
	 * current is MVP. update its runtime before deciding the
	 * preemption.
	 */
	walt_cfs_account_mvp_runtime(rq, c);
	resched = (rq->wrq.mvp_tasks.next != &wts_c->wts.mvp_list);

	/*
	 * current is no longer eligible to run. It must have been
	 * picked (because of MVP) ahead of other tasks in the CFS
	 * tree, so drive preemption to pick up the next task from
	 * the tree, which also includes picking up the first in
	 * the MVP queue.
	 */
	if (resched)
		goto preempt;

	/* current is the first in the queue, so no preemption */
	*nopreempt = true;
	trace_walt_cfs_mvp_wakeup_nopreempt(c, &wts_c->wts, walt_cfs_mvp_task_limit(c));
	return;
preempt:
	*preempt = true;
	trace_walt_cfs_mvp_wakeup_preempt(p, &wts_p->wts, walt_cfs_mvp_task_limit(p));
}

static void
walt_select_task_rq_fair(void *unused, struct task_struct *p, int prev_cpu,
				int sd_flag, int wake_flags, int *target_cpu)
{
	int sync;
	int sibling_count_hint;

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
	sibling_count_hint = 1;

	*target_cpu = find_energy_efficient_cpu(p, prev_cpu, sync, sibling_count_hint);
}

void walt_cfs_tick(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	raw_spin_lock(&rq->lock);

	if (list_empty(&p->wts.mvp_list) || (p->wts.mvp_list.next == NULL))
		goto out;

	walt_cfs_account_mvp_runtime(rq, rq->curr);
	/*
	 * If the current is not MVP means, we have to re-schedule to
	 * see if we can run any other task including MVP tasks.
	 */
	if ((rq->wrq.mvp_tasks.next != &p->wts.mvp_list) && rq->cfs.h_nr_running > 1)
		resched_curr(rq);

out:
	raw_spin_unlock(&rq->lock);
}

static void walt_binder_low_latency_set(void *unused, struct task_struct *task,
					bool sync, struct binder_proc *proc)
{
	if (task && ((task_in_related_thread_group(current) &&
			task->group_leader->prio < MAX_RT_PRIO) ||
			(current->group_leader->prio < MAX_RT_PRIO &&
			task_in_related_thread_group(task))))
		task->wts.low_latency |= WALT_LOW_LATENCY_BINDER;
	else
		/*
		 * Clear low_latency flag if criterion above is not met, this
		 * will handle usecase where for a binder thread WALT_LOW_LATENCY_BINDER
		 * is set by one task and before WALT clears this flag after timer expiry
		 * some other task tries to use same binder thread.
		 *
		 * The only gets cleared when binder transaction is initiated
		 * and the above condition to set flasg is nto satisfied.
		 */
		task->wts.low_latency &= ~WALT_LOW_LATENCY_BINDER;

}

static void binder_set_priority_hook(void *data,
				struct binder_transaction *bndrtrans, struct task_struct *task)
{

	if (bndrtrans && bndrtrans->need_reply && current->wts.boost == TASK_BOOST_STRICT_MAX) {
		bndrtrans->android_vendor_data1  = task->wts.boost;
		task->wts.boost = TASK_BOOST_STRICT_MAX;
	}
}

static void binder_restore_priority_hook(void *data,
				struct binder_transaction *bndrtrans, struct task_struct *task)
{
	if (bndrtrans && task->wts.boost == TASK_BOOST_STRICT_MAX)
		task->wts.boost = bndrtrans->android_vendor_data1;
}

#ifdef CONFIG_FAIR_GROUP_SCHED
/* Walk up scheduling entities hierarchy */
#define for_each_sched_entity(se) \
		for (; se; se = se->parent)
#else	/* !CONFIG_FAIR_GROUP_SCHED */
#define for_each_sched_entity(se) \
		for (; se; se = NULL)
#endif

/* runqueue on which this entity is (to be) queued */
static inline struct cfs_rq *cfs_rq_of(struct sched_entity *se)
{
	return se->cfs_rq;
}

extern void set_next_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);
static void walt_cfs_replace_next_task_fair(void *unused, struct rq *rq, struct task_struct **p,
					    struct sched_entity **se, bool *repick, bool simple,
					    struct task_struct *prev)
{
	struct task_struct *wts;
	struct task_struct *mvp;
	struct cfs_rq *cfs_rq;

	if (unlikely(walt_disabled))
		return;

	/* We don't have MVP tasks queued */
	if (list_empty(&rq->wrq.mvp_tasks))
		return;

	/* Return the first task from MVP queue */
	wts = list_first_entry(&rq->wrq.mvp_tasks, struct task_struct, wts.mvp_list);
	mvp = wts_to_ts(&wts);

	*p = mvp;
	*se = &mvp->se;
	*repick = true;

	if (simple) {
		for_each_sched_entity((*se)) {
			/*
			 * TODO If CFS_BANDWIDTH is enabled, we might pick
			 * from a throttled cfs_rq
			 */
			cfs_rq = cfs_rq_of(*se);
			set_next_entity(cfs_rq, *se);
		}
	}

	trace_walt_cfs_mvp_pick_next(mvp, &wts->wts, walt_cfs_mvp_task_limit(mvp));
}

void walt_cfs_init(void)
{
	register_trace_android_rvh_select_task_rq_fair(walt_select_task_rq_fair, NULL);

	register_trace_android_vh_binder_wakeup_ilocked(walt_binder_low_latency_set, NULL);

	register_trace_android_vh_binder_set_priority(binder_set_priority_hook, NULL);
	register_trace_android_vh_binder_restore_priority(binder_restore_priority_hook, NULL);

	register_trace_android_rvh_check_preempt_wakeup(walt_cfs_check_preempt_wakeup, NULL);
/*
	register_trace_android_rvh_replace_next_task_fair(walt_cfs_replace_next_task_fair, NULL);
*/
}
#endif /* CONFIG_SCHED_WALT */
