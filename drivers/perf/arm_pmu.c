#undef DEBUG

/*
 * ARM performance counter support.
 *
 * Copyright (C) 2009 picoChip Designs, Ltd., Jamie Iles
 * Copyright (C) 2010 ARM Ltd., Will Deacon <will.deacon@arm.com>
 *
 * This code is based on the sparc64 perf event code, which is in turn based
 * on the x86 code.
 */
#define pr_fmt(fmt) "hw perfevents: " fmt

#include <linux/bitmap.h>
#include <linux/cpumask.h>
#include <linux/cpu_pm.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/perf/arm_pmu.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sched/clock.h>
#include <linux/spinlock.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>

#include <asm/irq_regs.h>

#define USE_CPUHP_STATE CPUHP_AP_PERF_ARM_STARTING
#define USE_CPUHP_STR "AP_PERF_ARM_STARTING"

static int
armpmu_map_cache_event(const unsigned (*cache_map)
				      [PERF_COUNT_HW_CACHE_MAX]
				      [PERF_COUNT_HW_CACHE_OP_MAX]
				      [PERF_COUNT_HW_CACHE_RESULT_MAX],
		       u64 config)
{
	unsigned int cache_type, cache_op, cache_result, ret;

	cache_type = (config >>  0) & 0xff;
	if (cache_type >= PERF_COUNT_HW_CACHE_MAX)
		return -EINVAL;

	cache_op = (config >>  8) & 0xff;
	if (cache_op >= PERF_COUNT_HW_CACHE_OP_MAX)
		return -EINVAL;

	cache_result = (config >> 16) & 0xff;
	if (cache_result >= PERF_COUNT_HW_CACHE_RESULT_MAX)
		return -EINVAL;

	if (!cache_map)
		return -ENOENT;

	ret = (int)(*cache_map)[cache_type][cache_op][cache_result];

	if (ret == CACHE_OP_UNSUPPORTED)
		return -ENOENT;

	return ret;
}

static int
armpmu_map_hw_event(const unsigned (*event_map)[PERF_COUNT_HW_MAX], u64 config)
{
	int mapping;

	if (config >= PERF_COUNT_HW_MAX)
		return -EINVAL;

	if (!event_map)
		return -ENOENT;

	mapping = (*event_map)[config];
	return mapping == HW_OP_UNSUPPORTED ? -ENOENT : mapping;
}

static int
armpmu_map_raw_event(u32 raw_event_mask, u64 config)
{
	return (int)(config & raw_event_mask);
}

int
armpmu_map_event(struct perf_event *event,
		 const unsigned (*event_map)[PERF_COUNT_HW_MAX],
		 const unsigned (*cache_map)
				[PERF_COUNT_HW_CACHE_MAX]
				[PERF_COUNT_HW_CACHE_OP_MAX]
				[PERF_COUNT_HW_CACHE_RESULT_MAX],
		 u32 raw_event_mask)
{
	u64 config = event->attr.config;
	int type = event->attr.type;

	if (type == event->pmu->type)
		return armpmu_map_raw_event(raw_event_mask, config);

	switch (type) {
	case PERF_TYPE_HARDWARE:
		return armpmu_map_hw_event(event_map, config);
	case PERF_TYPE_HW_CACHE:
		return armpmu_map_cache_event(cache_map, config);
	case PERF_TYPE_RAW:
		return armpmu_map_raw_event(raw_event_mask, config);
	}

	return -ENOENT;
}

int armpmu_event_set_period(struct perf_event *event)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	s64 left = local64_read(&hwc->period_left);
	s64 period = hwc->sample_period;
	int ret = 0;

	if (unlikely(left <= -period)) {
		left = period;
		local64_set(&hwc->period_left, left);
		hwc->last_period = period;
		ret = 1;
	}

	if (unlikely(left <= 0)) {
		left += period;
		local64_set(&hwc->period_left, left);
		hwc->last_period = period;
		ret = 1;
	}

	/*
	 * Limit the maximum period to prevent the counter value
	 * from overtaking the one we are about to program. In
	 * effect we are reducing max_period to account for
	 * interrupt latency (and we are being very conservative).
	 */
	if (left > (armpmu->max_period >> 1))
		left = armpmu->max_period >> 1;

	local64_set(&hwc->prev_count, (u64)-left);

	armpmu->write_counter(event, (u64)(-left) & 0xffffffff);

	perf_event_update_userpage(event);

	return ret;
}

u64 armpmu_event_update(struct perf_event *event)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	u64 delta, prev_raw_count, new_raw_count;

again:
	prev_raw_count = local64_read(&hwc->prev_count);
	new_raw_count = armpmu->read_counter(event);

	if (local64_cmpxchg(&hwc->prev_count, prev_raw_count,
			     new_raw_count) != prev_raw_count)
		goto again;

	delta = (new_raw_count - prev_raw_count) & armpmu->max_period;

	local64_add(delta, &event->count);
	local64_sub(delta, &hwc->period_left);

	return new_raw_count;
}

static void
armpmu_read(struct perf_event *event)
{
	armpmu_event_update(event);
}

static void
armpmu_stop(struct perf_event *event, int flags)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	/*
	 * ARM pmu always has to update the counter, so ignore
	 * PERF_EF_UPDATE, see comments in armpmu_start().
	 */
	if (!(hwc->state & PERF_HES_STOPPED)) {
		armpmu->disable(event);
		armpmu_event_update(event);
		hwc->state |= PERF_HES_STOPPED | PERF_HES_UPTODATE;
	}
}

static void armpmu_start(struct perf_event *event, int flags)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;

	/*
	 * ARM pmu always has to reprogram the period, so ignore
	 * PERF_EF_RELOAD, see the comment below.
	 */
	if (flags & PERF_EF_RELOAD)
		WARN_ON_ONCE(!(hwc->state & PERF_HES_UPTODATE));

	hwc->state = 0;
	/*
	 * Set the period again. Some counters can't be stopped, so when we
	 * were stopped we simply disabled the IRQ source and the counter
	 * may have been left counting. If we don't do this step then we may
	 * get an interrupt too soon or *way* too late if the overflow has
	 * happened since disabling.
	 */
	armpmu_event_set_period(event);
	armpmu->enable(event);
}

static void
armpmu_del(struct perf_event *event, int flags)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct pmu_hw_events *hw_events = this_cpu_ptr(armpmu->hw_events);
	struct hw_perf_event *hwc = &event->hw;
	int idx = hwc->idx;

	armpmu_stop(event, PERF_EF_UPDATE);
	hw_events->events[idx] = NULL;
	clear_bit(idx, hw_events->used_mask);
	if (armpmu->clear_event_idx)
		armpmu->clear_event_idx(hw_events, event);

	perf_event_update_userpage(event);
}

static int
armpmu_add(struct perf_event *event, int flags)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct pmu_hw_events *hw_events = this_cpu_ptr(armpmu->hw_events);
	struct hw_perf_event *hwc = &event->hw;
	int idx;

	/* An event following a process won't be stopped earlier */
	if (!cpumask_test_cpu(smp_processor_id(), &armpmu->supported_cpus))
		return -ENOENT;

	/* If we don't have a space for the counter then finish early. */
	idx = armpmu->get_event_idx(hw_events, event);
	if (idx < 0)
		return idx;

	/* The newly-allocated counter should be empty */
	WARN_ON_ONCE(hw_events->events[idx]);

	event->hw.idx = idx;
	hw_events->events[idx] = event;

	hwc->state = PERF_HES_STOPPED | PERF_HES_UPTODATE;
	if (flags & PERF_EF_START)
		armpmu_start(event, PERF_EF_RELOAD);

	/* Propagate our changes to the userspace mapping. */
	perf_event_update_userpage(event);

	return 0;
}

static int
validate_event(struct pmu *pmu, struct pmu_hw_events *hw_events,
			       struct perf_event *event)
{
	struct arm_pmu *armpmu;

	if (is_software_event(event))
		return 1;

	/*
	 * Reject groups spanning multiple HW PMUs (e.g. CPU + CCI). The
	 * core perf code won't check that the pmu->ctx == leader->ctx
	 * until after pmu->event_init(event).
	 */
	if (event->pmu != pmu)
		return 0;

	if (event->state < PERF_EVENT_STATE_OFF)
		return 1;

	if (event->state == PERF_EVENT_STATE_OFF && !event->attr.enable_on_exec)
		return 1;

	armpmu = to_arm_pmu(event->pmu);
	return armpmu->get_event_idx(hw_events, event) >= 0;
}

static int
validate_group(struct perf_event *event)
{
	struct perf_event *sibling, *leader = event->group_leader;
	struct pmu_hw_events fake_pmu;

	/*
	 * Initialise the fake PMU. We only need to populate the
	 * used_mask for the purposes of validation.
	 */
	memset(&fake_pmu.used_mask, 0, sizeof(fake_pmu.used_mask));

	if (!validate_event(event->pmu, &fake_pmu, leader))
		return -EINVAL;

	list_for_each_entry(sibling, &leader->sibling_list, group_entry) {
		if (!validate_event(event->pmu, &fake_pmu, sibling))
			return -EINVAL;
	}

	if (!validate_event(event->pmu, &fake_pmu, event))
		return -EINVAL;

	return 0;
}

static struct arm_pmu_platdata *armpmu_get_platdata(struct arm_pmu *armpmu)
{
	struct platform_device *pdev = armpmu->plat_device;

	return pdev ? dev_get_platdata(&pdev->dev) : NULL;
}

static irqreturn_t armpmu_dispatch_irq(int irq, void *dev)
{
	struct arm_pmu *armpmu;
	struct arm_pmu_platdata *plat;
	int ret;
	u64 start_clock, finish_clock;

	/*
	 * we request the IRQ with a (possibly percpu) struct arm_pmu**, but
	 * the handlers expect a struct arm_pmu*. The percpu_irq framework will
	 * do any necessary shifting, we just need to perform the first
	 * dereference.
	 */
	armpmu = *(void **)dev;

	plat = armpmu_get_platdata(armpmu);

	start_clock = sched_clock();
	if (plat && plat->handle_irq)
		ret = plat->handle_irq(irq, armpmu, armpmu->handle_irq);
	else
		ret = armpmu->handle_irq(irq, armpmu);
	finish_clock = sched_clock();

	perf_sample_event_took(finish_clock - start_clock);
	return ret;
}

static int
event_requires_mode_exclusion(struct perf_event_attr *attr)
{
	return attr->exclude_idle || attr->exclude_user ||
	       attr->exclude_kernel || attr->exclude_hv;
}

static int
__hw_perf_event_init(struct perf_event *event)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	struct hw_perf_event *hwc = &event->hw;
	int mapping;

	mapping = armpmu->map_event(event);

	if (mapping < 0) {
		pr_debug("event %x:%llx not supported\n", event->attr.type,
			 event->attr.config);
		return mapping;
	}

	/*
	 * We don't assign an index until we actually place the event onto
	 * hardware. Use -1 to signify that we haven't decided where to put it
	 * yet. For SMP systems, each core has it's own PMU so we can't do any
	 * clever allocation or constraints checking at this point.
	 */
	hwc->idx		= -1;
	hwc->config_base	= 0;
	hwc->config		= 0;
	hwc->event_base		= 0;

	/*
	 * Check whether we need to exclude the counter from certain modes.
	 */
	if ((!armpmu->set_event_filter ||
	     armpmu->set_event_filter(hwc, &event->attr)) &&
	     event_requires_mode_exclusion(&event->attr)) {
		pr_debug("ARM performance counters do not support "
			 "mode exclusion\n");
		return -EOPNOTSUPP;
	}

	/*
	 * Store the event encoding into the config_base field.
	 */
	hwc->config_base	    |= (unsigned long)mapping;

	if (!is_sampling_event(event)) {
		/*
		 * For non-sampling runs, limit the sample_period to half
		 * of the counter width. That way, the new counter value
		 * is far less likely to overtake the previous one unless
		 * you have some serious IRQ latency issues.
		 */
		hwc->sample_period  = armpmu->max_period >> 1;
		hwc->last_period    = hwc->sample_period;
		local64_set(&hwc->period_left, hwc->sample_period);
	}

	if (event->group_leader != event) {
		if (validate_group(event) != 0)
			return -EINVAL;
	}

	return 0;
}

static int armpmu_event_init(struct perf_event *event)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);

	/*
	 * Reject CPU-affine events for CPUs that are of a different class to
	 * that which this PMU handles. Process-following events (where
	 * event->cpu == -1) can be migrated between CPUs, and thus we have to
	 * reject them later (in armpmu_add) if they're scheduled on a
	 * different class of CPU.
	 */
	if (event->cpu != -1 &&
		!cpumask_test_cpu(event->cpu, &armpmu->supported_cpus))
		return -ENOENT;

	/* does not support taken branch sampling */
	if (has_branch_stack(event))
		return -EOPNOTSUPP;

	if (armpmu->map_event(event) == -ENOENT)
		return -ENOENT;

	return __hw_perf_event_init(event);
}

static void armpmu_enable(struct pmu *pmu)
{
	struct arm_pmu *armpmu = to_arm_pmu(pmu);
	struct pmu_hw_events *hw_events = this_cpu_ptr(armpmu->hw_events);
	int enabled = bitmap_weight(hw_events->used_mask, armpmu->num_events);

	/* For task-bound events we may be called on other CPUs */
	if (!cpumask_test_cpu(smp_processor_id(), &armpmu->supported_cpus))
		return;

	if (enabled)
		armpmu->start(armpmu);
}

static void armpmu_disable(struct pmu *pmu)
{
	struct arm_pmu *armpmu = to_arm_pmu(pmu);

	/* For task-bound events we may be called on other CPUs */
	if (!cpumask_test_cpu(smp_processor_id(), &armpmu->supported_cpus))
		return;

	armpmu->stop(armpmu);
}

/*
 * In heterogeneous systems, events are specific to a particular
 * microarchitecture, and aren't suitable for another. Thus, only match CPUs of
 * the same microarchitecture.
 */
static int armpmu_filter_match(struct perf_event *event)
{
	struct arm_pmu *armpmu = to_arm_pmu(event->pmu);
	unsigned int cpu = smp_processor_id();
	int ret;

	ret = cpumask_test_cpu(cpu, &armpmu->supported_cpus);
	if (ret && armpmu->filter_match)
		return armpmu->filter_match(event);

	return ret;
}

static ssize_t armpmu_cpumask_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct arm_pmu *armpmu = to_arm_pmu(dev_get_drvdata(dev));
	return cpumap_print_to_pagebuf(true, buf, &armpmu->supported_cpus);
}

static DEVICE_ATTR(cpus, S_IRUGO, armpmu_cpumask_show, NULL);

static struct attribute *armpmu_common_attrs[] = {
	&dev_attr_cpus.attr,
	NULL,
};

static struct attribute_group armpmu_common_attr_group = {
	.attrs = armpmu_common_attrs,
};

/* Set at runtime when we know what CPU type we are. */
static struct arm_pmu *__oprofile_cpu_pmu;

/*
 * Despite the names, these two functions are CPU-specific and are used
 * by the OProfile/perf code.
 */
const char *perf_pmu_name(void)
{
	if (!__oprofile_cpu_pmu)
		return NULL;

	return __oprofile_cpu_pmu->name;
}
EXPORT_SYMBOL_GPL(perf_pmu_name);

int perf_num_counters(void)
{
	int max_events = 0;

	if (__oprofile_cpu_pmu != NULL)
		max_events = __oprofile_cpu_pmu->num_events;

	return max_events;
}
EXPORT_SYMBOL_GPL(perf_num_counters);

void armpmu_free_irq(struct arm_pmu *armpmu, int cpu)
{
	struct pmu_hw_events __percpu *hw_events = armpmu->hw_events;
	int irq = per_cpu(hw_events->irq, cpu);

	if (!cpumask_test_and_clear_cpu(cpu, &armpmu->active_irqs))
		return;

	armpmu->pmu_state = ARM_PMU_STATE_GOING_DOWN;

	if (irq_is_percpu(irq)) {
		free_percpu_irq(irq, &hw_events->percpu_pmu);
		cpumask_clear(&armpmu->active_irqs);
		armpmu->percpu_irq = -1;
		armpmu->pmu_state = ARM_PMU_STATE_OFF;
		return;
	}

	free_irq(irq, per_cpu_ptr(&hw_events->percpu_pmu, cpu));
	armpmu->pmu_state = ARM_PMU_STATE_OFF;
}

void armpmu_free_irqs(struct arm_pmu *armpmu)
{
	int cpu;

	for_each_cpu(cpu, &armpmu->supported_cpus)
		armpmu_free_irq(armpmu, cpu);
}

int armpmu_request_irq(struct arm_pmu *armpmu, int cpu)
{
	int err = 0;
	struct pmu_hw_events __percpu *hw_events = armpmu->hw_events;
	const irq_handler_t handler = armpmu_dispatch_irq;
	int irq = per_cpu(hw_events->irq, cpu);
	if (!irq)
		return 0;

	if (irq_is_percpu(irq) && cpumask_empty(&armpmu->active_irqs)) {
		err = request_percpu_irq(irq, handler, "arm-pmu",
					 &hw_events->percpu_pmu);
		armpmu->percpu_irq = irq;
	} else if (irq_is_percpu(irq)) {
		int other_cpu = cpumask_first(&armpmu->active_irqs);
		int other_irq = per_cpu(hw_events->irq, other_cpu);

		if (irq != other_irq) {
			pr_warn("mismatched PPIs detected.\n");
			err = -EINVAL;
			goto err_out;
		}
	} else {
		struct arm_pmu_platdata *platdata = armpmu_get_platdata(armpmu);
		unsigned long irq_flags;

		err = irq_force_affinity(irq, cpumask_of(cpu));

		if (err && num_possible_cpus() > 1) {
			pr_warn("unable to set irq affinity (irq=%d, cpu=%u)\n",
				irq, cpu);
			goto err_out;
		}

		if (platdata && platdata->irq_flags) {
			irq_flags = platdata->irq_flags;
		} else {
			irq_flags = IRQF_PERCPU |
				    IRQF_NOBALANCING |
				    IRQF_NO_THREAD;
		}

		err = request_irq(irq, handler, irq_flags, "arm-pmu",
				  per_cpu_ptr(&hw_events->percpu_pmu, cpu));
	}

	if (err)
		goto err_out;

	armpmu->pmu_state = ARM_PMU_STATE_RUNNING;

	cpumask_set_cpu(cpu, &armpmu->active_irqs);
	return 0;

err_out:
	pr_err("unable to request IRQ%d for ARM PMU counters\n", irq);
	return err;
}

int armpmu_request_irqs(struct arm_pmu *armpmu)
{
	int cpu, err;

	for_each_cpu(cpu, &armpmu->supported_cpus) {
		err = armpmu_request_irq(armpmu, cpu);
		if (err)
			break;
	}

	return err;
}

struct cpu_pm_pmu_args {
	struct arm_pmu	*armpmu;
	unsigned long	cmd;
	int		cpu;
	int		ret;
};

#ifdef CONFIG_CPU_PM
static void cpu_pm_pmu_setup(struct arm_pmu *armpmu, unsigned long cmd)
{
	struct pmu_hw_events *hw_events = this_cpu_ptr(armpmu->hw_events);
	struct perf_event *event;
	int idx;

	for (idx = 0; idx < armpmu->num_events; idx++) {
		/*
		 * If the counter is not used skip it, there is no
		 * need of stopping/restarting it.
		 */
		if (!test_bit(idx, hw_events->used_mask))
			continue;

		event = hw_events->events[idx];
		if (!event)
			continue;

		if (event->state != PERF_EVENT_STATE_ACTIVE)
			continue;

		switch (cmd) {
		case CPU_PM_ENTER:
			/*
			 * Stop and update the counter
			 */
			armpmu_stop(event, PERF_EF_UPDATE);
			break;
		case CPU_PM_EXIT:
		case CPU_PM_ENTER_FAILED:
			 /*
			  * Restore and enable the counter.
			  * armpmu_start() indirectly calls
			  *
			  * perf_event_update_userpage()
			  *
			  * that requires RCU read locking to be functional,
			  * wrap the call within RCU_NONIDLE to make the
			  * RCU subsystem aware this cpu is not idle from
			  * an RCU perspective for the armpmu_start() call
			  * duration.
			  */
			RCU_NONIDLE(armpmu_start(event, PERF_EF_RELOAD));
			break;
		default:
			break;
		}
	}
}

static void cpu_pm_pmu_common(void *info)
{
	struct cpu_pm_pmu_args *data	= info;
	struct arm_pmu *armpmu		= data->armpmu;
	unsigned long cmd		= data->cmd;
	int cpu				= data->cpu;
	struct pmu_hw_events *hw_events = this_cpu_ptr(armpmu->hw_events);
	int enabled = bitmap_weight(hw_events->used_mask, armpmu->num_events);

	if (!cpumask_test_cpu(cpu, &armpmu->supported_cpus)) {
		data->ret = NOTIFY_DONE;
		return;
	}

	/*
	 * Always reset the PMU registers on power-up even if
	 * there are no events running.
	 */
	if (cmd == CPU_PM_EXIT && armpmu->reset)
		armpmu->reset(armpmu);

	if (!enabled) {
		data->ret = NOTIFY_OK;
		return;
	}

	data->ret = NOTIFY_OK;

	switch (cmd) {
	case CPU_PM_ENTER:
		armpmu->stop(armpmu);
		cpu_pm_pmu_setup(armpmu, cmd);
		break;
	case CPU_PM_EXIT:
	case CPU_PM_ENTER_FAILED:
		cpu_pm_pmu_setup(armpmu, cmd);
		armpmu->start(armpmu);
		break;
	default:
		data->ret = NOTIFY_DONE;
		break;
	}

	return;
}

static int cpu_pm_pmu_notify(struct notifier_block *b, unsigned long cmd,
			     void *v)
{
	struct cpu_pm_pmu_args data = {
		.armpmu	= container_of(b, struct arm_pmu, cpu_pm_nb),
		.cmd	= cmd,
		.cpu	= smp_processor_id(),
	};

	cpu_pm_pmu_common(&data);
	return data.ret;
}

static int cpu_pm_pmu_register(struct arm_pmu *cpu_pmu)
{
	cpu_pmu->cpu_pm_nb.notifier_call = cpu_pm_pmu_notify;
	return cpu_pm_register_notifier(&cpu_pmu->cpu_pm_nb);
}

static void cpu_pm_pmu_unregister(struct arm_pmu *cpu_pmu)
{
	cpu_pm_unregister_notifier(&cpu_pmu->cpu_pm_nb);
}

#else
static inline int cpu_pm_pmu_register(struct arm_pmu *cpu_pmu) { return 0; }
static inline void cpu_pm_pmu_unregister(struct arm_pmu *cpu_pmu) { }
static void cpu_pm_pmu_common(void *info) { }
#endif

/*
 * PMU hardware loses all context when a CPU goes offline.
 * When a CPU is hotplugged back in, since some hardware registers are
 * UNKNOWN at reset, the PMU must be explicitly reset to avoid reading
 * junk values out of them.
 */
static int arm_perf_starting_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct arm_pmu *pmu = hlist_entry_safe(node, struct arm_pmu, node);

	struct cpu_pm_pmu_args data = {
		.armpmu	= pmu,
		.cpu	= (int)cpu,
	};

	if (!pmu || !cpumask_test_cpu(cpu, &pmu->supported_cpus))
		return 0;

	if (pmu->reset)
		pmu->reset(pmu);

	if (data.armpmu->pmu_state != ARM_PMU_STATE_OFF &&
		data.armpmu->plat_device) {
		int irq = data.armpmu->percpu_irq;

		if (irq > 0 && irq_is_percpu(irq))
			enable_percpu_irq(irq, IRQ_TYPE_NONE);

	}

	return 0;
}

static int arm_perf_stopping_cpu(unsigned int cpu, struct hlist_node *node)
{
	struct arm_pmu *pmu = hlist_entry_safe(node, struct arm_pmu, node);

	struct cpu_pm_pmu_args data = {
		.armpmu	= pmu,
		.cpu	= (int)cpu,
	};

	if (!pmu || !cpumask_test_cpu(cpu, &pmu->supported_cpus))
		return 0;

	/* Disarm the PMU IRQ before disappearing. */
	if (data.armpmu->pmu_state == ARM_PMU_STATE_RUNNING &&
		data.armpmu->plat_device) {
		int irq = data.armpmu->percpu_irq;

		if (irq > 0 && irq_is_percpu(irq))
			disable_percpu_irq(irq);

	}

	return 0;
}

static int cpu_pmu_init(struct arm_pmu *cpu_pmu)
{
	int err;

	err = cpuhp_state_add_instance(CPUHP_AP_PERF_ARM_STARTING,
				       &cpu_pmu->node);
	if (err)
		goto out;

	err = cpu_pm_pmu_register(cpu_pmu);
	if (err)
		goto out_unreg_perf_starting;

	return 0;

out_unreg_perf_starting:
	cpuhp_state_remove_instance_nocalls(USE_CPUHP_STATE,
					    &cpu_pmu->node);
out:
	return err;
}

static void cpu_pmu_destroy(struct arm_pmu *cpu_pmu)
{
	cpu_pm_pmu_unregister(cpu_pmu);
	cpuhp_state_remove_instance_nocalls(USE_CPUHP_STATE,
					    &cpu_pmu->node);
}

struct arm_pmu *armpmu_alloc(void)
{
	struct arm_pmu *pmu;
	int cpu;

	pmu = kzalloc(sizeof(*pmu), GFP_KERNEL);
	if (!pmu) {
		pr_info("failed to allocate PMU device!\n");
		goto out;
	}

	pmu->hw_events = alloc_percpu(struct pmu_hw_events);
	if (!pmu->hw_events) {
		pr_info("failed to allocate per-cpu PMU data.\n");
		goto out_free_pmu;
	}

	pmu->pmu = (struct pmu) {
		.pmu_enable		= armpmu_enable,
		.pmu_disable		= armpmu_disable,
		.event_init		= armpmu_event_init,
		.add			= armpmu_add,
		.del			= armpmu_del,
		.start			= armpmu_start,
		.stop			= armpmu_stop,
		.read			= armpmu_read,
		.filter_match		= armpmu_filter_match,
		.attr_groups		= pmu->attr_groups,
		/*
		 * This is a CPU PMU potentially in a heterogeneous
		 * configuration (e.g. big.LITTLE). This is not an uncore PMU,
		 * and we have taken ctx sharing into account (e.g. with our
		 * pmu::filter_match callback and pmu::event_init group
		 * validation).
		 */
		.capabilities		= PERF_PMU_CAP_HETEROGENEOUS_CPUS,
		.events_across_hotplug	= 1,
	};

	pmu->attr_groups[ARMPMU_ATTR_GROUP_COMMON] =
		&armpmu_common_attr_group;

	for_each_possible_cpu(cpu) {
		struct pmu_hw_events *events;

		events = per_cpu_ptr(pmu->hw_events, cpu);
		raw_spin_lock_init(&events->pmu_lock);
		events->percpu_pmu = pmu;
	}

	pmu->pmu_state  = ARM_PMU_STATE_OFF;
	pmu->percpu_irq = -1;

	return pmu;

out_free_pmu:
	kfree(pmu);
out:
	return NULL;
}

void armpmu_free(struct arm_pmu *pmu)
{
	free_percpu(pmu->hw_events);
	kfree(pmu);
}

int armpmu_register(struct arm_pmu *pmu)
{
	int ret;

	ret = cpu_pmu_init(pmu);
	if (ret)
		return ret;

	ret = perf_pmu_register(&pmu->pmu, pmu->name, -1);
	if (ret)
		goto out_destroy;

	if (!__oprofile_cpu_pmu)
		__oprofile_cpu_pmu = pmu;

	pr_info("enabled with %s PMU driver, %d counters available\n",
		pmu->name, pmu->num_events);

	return 0;

out_destroy:
	cpu_pmu_destroy(pmu);
	return ret;
}

static int arm_pmu_hp_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_PERF_ARM_STARTING,
				      "perf/arm/pmu:starting",
				      arm_perf_starting_cpu,
				      arm_perf_stopping_cpu);
	if (ret)
		pr_err("CPU hotplug ARM PMU STOPPING registering failed: %d\n",
		       ret);
	return ret;
}
subsys_initcall(arm_pmu_hp_init);
