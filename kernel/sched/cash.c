// SPDX-License-Identifier: GPL-2.0
/*
 *  Cache-Aware Scheduling Heuristic (CASH)
 *  Copyright (C) 2025 shygosh <shygosh@proton.me>
 */

static unsigned int sched_cash_aggro_ns __read_mostly = 9000000;

struct cash_cpu {
	long factor;
	int cpu;
};

struct cash_group {
	long factor;
	struct sched_group *groups;
};

bool cash_active __read_mostly;
static bool cash_sg __read_mostly;

static struct cpumask *cash_find_best_group(struct sched_group *start,
					    struct cpumask *scope_mask)
{
	if (!READ_ONCE(cash_sg))
		return sched_group_span(start);
	struct sched_group *groups = start;
	struct cpumask *group_mask;
	struct cash_group best;

	best.factor = INT_MIN;

	do {
		long factor;

		group_mask = sched_group_span(groups);
		if (unlikely(!cpumask_intersects(group_mask, scope_mask)))
			continue;

		factor = READ_ONCE(groups->factor);
		if (factor > best.factor) {
			best.factor = factor;
			best.groups = groups;
		}
	} while ((groups = groups->next) != start);

	return group_mask;
}

static int cash_select_task_rq_fair(struct task_struct *p, int prev_cpu, int wake_flags)
{
	if (unlikely(!READ_ONCE(cash_active)))
		return select_task_rq_fair(p, prev_cpu, wake_flags);
	int cpu, p_cpu, p_que, this_cpu = raw_smp_processor_id();
	unsigned long p_est;
	struct rq *rq;
	struct cpumask group_mask, scope_mask;
	struct cash_cpu best;

	if (unlikely(!cpumask_and(&scope_mask, cpu_active_mask, p->cpus_ptr)))
		return cpumask_first(p->cpus_ptr);

	if (wake_flags & WF_TTWU) {
		record_wakee(p);

		if ((wake_flags & WF_CURRENT_CPU) &&
		    cpumask_test_cpu(this_cpu, &scope_mask))
			return this_cpu;

		if (is_per_cpu_kthread(current) && in_task() &&
		    prev_cpu == this_cpu && this_rq()->nr_running <= 1)
			return prev_cpu;

		if (!wake_wide(p))
			wake_flags |= WF_AFFINE | WF_AGGRO;
		else if ((sched_clock() - smp_load_acquire(&p->last_ts)) <
			 (u64)sched_cash_aggro_ns)
			wake_flags |= WF_AGGRO;
	}

	if (!(wake_flags & WF_FORK))
		sync_entity_load_avg(&p->se);
	p_est = _task_util_est(p);
	rq = cpu_rq(prev_cpu);

redo:
	p_que = current == p || task_on_rq_queued(p);
	p_cpu = task_cpu(p);

	if (wake_flags & WF_AFFINE) {
		cpumask_and(&group_mask, cpu_smt_mask(prev_cpu), cpu_smt_mask(this_cpu));
		if (unlikely(!cpumask_and(&group_mask, &scope_mask, &group_mask))) {
			wake_flags &= ~WF_AFFINE;
			goto redo;
		}
	} else if (wake_flags & WF_AGGRO) {
		if (unlikely(!cpumask_and(&group_mask, &scope_mask, cpu_smt_mask(prev_cpu)))) {
			wake_flags &= ~WF_AGGRO;
			goto redo;
		}
	} else if (wake_flags & WF_TTWU) {
		if (unlikely(!cpumask_and(&group_mask, &scope_mask, sched_group_span(rq->groups))))
			cpumask_copy(&group_mask, cash_find_best_group(rq->groups, &scope_mask));
	} else {
		cpumask_copy(&group_mask, cash_find_best_group(rq->groups, &scope_mask));
	}

	best.factor = INT_MIN;
	for_each_cpu_wrap(cpu, &group_mask, prev_cpu + 2) {
		long factor;

		if (static_branch_unlikely(&sched_asym_cpucapacity))
			factor = arch_scale_cpu_capacity(cpu);
		else
			factor = SCHED_CAPACITY_SCALE;

		rq = cpu_rq(cpu);
		factor -= (long)(READ_ONCE(rq->avg_rt.util_avg) +
				 READ_ONCE(rq->cfs.avg.util_est));

		if (p_que && cpu == p_cpu)
			factor += p_est;

		if (factor > best.factor) {
			best.cpu = cpu;
			best.factor = factor;
		}
	}

	if ((wake_flags & (WF_AFFINE | WF_AGGRO)) &&
	    (best.factor - p_est < 128L)) {
		wake_flags &= ~(WF_AFFINE | WF_AGGRO);
		goto redo;
	}

	if (READ_ONCE(cash_sg) && !(wake_flags & (WF_AFFINE | WF_AGGRO)) &&
	    likely(cpumask_subset(&group_mask, &scope_mask))) {
		rq = cpu_rq(best.cpu);
		WRITE_ONCE(rq->groups->factor,
			(READ_ONCE(rq->groups->factor) * 3 + best.factor) >> 2);
	}

	return best.cpu;
}

void sched_cash_init(void)
{
	struct sched_domain *sd, *tmp;
	struct sched_group *groups;
	int cpu = smp_processor_id();

	WRITE_ONCE(cash_sg, false);

	for_each_domain(cpu, tmp) {
		sd = tmp;
	}

	groups = sd->groups;
	do {
		struct cpumask *group_mask = sched_group_span(groups);
		cpu = cpumask_first(group_mask);
		groups->factor = arch_scale_cpu_capacity(cpu);
		for_each_cpu(cpu, group_mask) {
			cpu_rq(cpu)->groups = groups;
		}
	} while ((groups = groups->next) != sd->groups);

	groups = sd->groups;
	do {
		struct cpumask *group_mask = sched_group_span(groups);
		cpu = cpumask_first(group_mask);
		if (cpumask_weight(group_mask) > cpumask_weight(cpu_smt_mask(cpu))) {
			WRITE_ONCE(cash_sg, true);
			break;
		}
	} while ((groups = groups->next) != sd->groups);

	smp_mb();
	pr_warn("sched_cash: group: active=%s\n", READ_ONCE(cash_sg) ? "true" : "false");
	pr_warn("sched_cash: domain: weight=%u level=%d\n", sd->span_weight, sd->level);
	WRITE_ONCE(cash_active, true);
}

#ifdef CONFIG_SYSCTL
static const struct ctl_table sched_cash_sysctls[] = {
	{
		.procname	= "sched_cash_aggro_ns",
		.data		= &sched_cash_aggro_ns,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec,
	},
};

static int __init sched_cash_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_cash_sysctls);
	return 0;
}
late_initcall(sched_cash_sysctl_init);
#endif
