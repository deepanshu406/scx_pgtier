/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_pgtier: step 3A of 7, per-cgroup records and queues (nothing uses the
 * queues yet), on top of step 2, the express lane, and step 1, tier detection.
 *
 * Step 3A (new): sched_ext tells us when a cgroup appears (cgroup_init) and
 * when it goes away (cgroup_exit).
 *   cgroup_init: read the cgroup's NAME, store a record on the cgroup
 *                (tier, queue id), and for a "ufs-bg-*" cgroup create its own
 *                queue (a DSQ). The queue id is the cgroup's own id.
 *   cgroup_exit: destroy that queue.
 * Nothing is put in these queues yet, and dispatch does not read them. Background
 * tasks still use the single shared FIFO queue. This step only proves that the
 * records and queues are created and destroyed correctly.
 *
 * NOTE: the cpu controller must be enabled for a cgroup to get its own
 * cgroup_init call (echo +cpu > /sys/fs/cgroup/cgroup.subtree_control).
 *
 * Step 1 (kept): when a task joins sched_ext (enable), read the NAME of its
 * cgroup. "ufs-ts-*" -> time-sensitive tier, "ufs-bg-*" -> background tier,
 * anything else -> counted as unknown and treated as background.
 *
 * Step 2 (new): time-sensitive tasks get a CPU quickly.
 *   select_cpu, time-sensitive task:
 *     1. idle CPU (previous one first)  -> insert straight into its local queue
 *     2. else a CPU recorded as running a background task -> return that CPU
 *     3. else the default CPU (all CPUs run time-sensitive work)
 *   enqueue, time-sensitive task, on the CPU chosen above:
 *     - CPU runs background work  -> insert at the HEAD of its local queue and
 *                                    kick it with PREEMPT (the express lane)
 *     - CPU runs time-sensitive   -> insert behind it, no kick (collision rule:
 *                                    time-sensitive tasks never preempt each other)
 *     - nothing recorded          -> insert and kick without preempt (wake it)
 *   Background tasks are unchanged: one shared FIFO queue.
 *
 * Not yet: virtual runtime, weights, using the per-cgroup queues (rest of step 3).
 *
 * Run it in PARTIAL mode: only tasks switched to SCHED_EXT are ours.
 *
 * The scheduling callbacks follow tools/sched_ext/scx_simple.bpf.c
 * (Copyright (c) 2022 Meta Platforms, Inc. and affiliates;
 *  Copyright (c) 2022 Tejun Heo <tj@kernel.org>;
 *  Copyright (c) 2022 David Vernet <dvernet@meta.com>).
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

#define SHARED_DSQ	0

/* Set to 0 to silence the trace_pipe output. */
#define UFS_DEBUG	1

/* Slice for time-sensitive tasks: short, so a task queued behind one waits little. */
#define TS_SLICE_NS	(2 * 1000 * 1000ULL)

#define MAX_CPUS	1024

/* Number of possible CPUs; the loader fills this in before loading. */
const volatile u32 nr_cpus = MAX_CPUS;

/* Tier stored on each task. */
enum ufs_tier {
	TIER_BG = 0,
	TIER_TS = 1,
};

/* What the name check found. */
enum ufs_class {
	CLASS_BG = 0,		/* name starts with "ufs-bg-" */
	CLASS_TS = 1,		/* name starts with "ufs-ts-" */
	CLASS_UNKNOWN = 2,	/* any other name, or the name could not be read */
	NR_CLASSES,
};

/* Index into the stats map. 0..NR_CLASSES-1 are the ufs_class values. */
enum ufs_stat {
	ST_TS_IDLE = NR_CLASSES,	/* ts task placed on an idle CPU in select_cpu */
	ST_TS_PREEMPT,			/* ts task jumped the queue and kicked a bg task */
	ST_TS_QUEUED,			/* ts task queued behind a running ts task */
	ST_TS_NUDGE,			/* ts task queued on a CPU with nothing recorded */
	ST_BG_ENQ,			/* bg task put on the shared queue */
	ST_CGRP_BG,			/* "ufs-bg-*" cgroup seen: record made, queue created */
	ST_CGRP_TS,			/* "ufs-ts-*" cgroup seen: record made, no queue */
	ST_CGRP_OTHER,			/* any other cgroup seen: record made, no queue */
	ST_CGRP_DESTROYED,		/* a cgroup queue was destroyed in cgroup_exit */
	ST_CGRP_ERR,			/* creating a record or queue failed */
	UFS_NR_STATS,
};

/* What a CPU is running, as recorded by running()/stopping(). 0 = nothing. */
enum ufs_cpu_state {
	CPU_NONE = 0,
	CPU_BG = 1,
	CPU_TS = 2,
};

#define NAME_LEN	32	/* we only need the first few characters */
#define PREFIX_LEN	7	/* strlen("ufs-ts-") == strlen("ufs-bg-") */

struct task_ctx {
	u8 tier;
};

struct {
	__uint(type, BPF_MAP_TYPE_TASK_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct task_ctx);
} task_ctxs SEC(".maps");

/* Per-cgroup record, created in cgroup_init. */
struct cgrp_ctx {
	u8  tier;	/* TIER_TS or TIER_BG */
	u8  has_dsq;	/* 1 if this cgroup owns a queue */
	u64 dsq_id;	/* the queue's id: the cgroup's own id */
};

struct {
	__uint(type, BPF_MAP_TYPE_CGRP_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct cgrp_ctx);
} cgrp_ctxs SEC(".maps");

/*
 * One entry per CPU. Written by the CPU itself in running()/stopping(), read by
 * other CPUs in select_cpu(). A plain array (not per-CPU) because we read other
 * CPUs' entries. Reads can be slightly stale; that is tolerated.
 */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, MAX_CPUS);
	__type(key, u32);
	__type(value, u8);
} cpu_state SEC(".maps");

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, UFS_NR_STATS);
} stats SEC(".maps");

static void stat_inc(u32 idx)
{
	u64 *cnt = bpf_map_lookup_elem(&stats, &idx);

	if (cnt)
		(*cnt)++;
}

static __always_inline u8 task_tier(struct task_struct *p)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);

	return tctx ? tctx->tier : TIER_BG;
}

static __always_inline u8 cpu_state_of(s32 cpu)
{
	u32 key = cpu;
	u8 *st = bpf_map_lookup_elem(&cpu_state, &key);

	return st ? *st : CPU_NONE;
}

static __always_inline void cpu_state_set(u8 state)
{
	u32 key = bpf_get_smp_processor_id();
	u8 *st = bpf_map_lookup_elem(&cpu_state, &key);

	if (st)
		*st = state;
}

/* True if the first PREFIX_LEN characters of name equal prefix. */
static __always_inline bool has_prefix(const char *name, const char *prefix)
{
	int i;

	for (i = 0; i < PREFIX_LEN; i++)
		if (name[i] != prefix[i])
			return false;
	return true;
}

/*
 * Copy the cgroup name that namep points to into name, and classify it by its
 * prefix.
 */
static __always_inline int classify_name(const char *namep, char *name)
{
	if (!namep)
		return CLASS_UNKNOWN;

	if (bpf_probe_read_kernel_str(name, NAME_LEN, namep) <= 0)
		return CLASS_UNKNOWN;

	if (has_prefix(name, "ufs-ts-"))
		return CLASS_TS;
	if (has_prefix(name, "ufs-bg-"))
		return CLASS_BG;
	return CLASS_UNKNOWN;
}

/*
 * Read the name of the task's cgroup (unified hierarchy) and classify it.
 * Reads p->cgroups->dfl_cgrp->kn->name with CO-RE. This needs no kfunc and no
 * reference counting.
 */
static __always_inline int classify(struct task_struct *p, char *name)
{
	return classify_name(BPF_CORE_READ(p, cgroups, dfl_cgrp, kn, name), name);
}

/*
 * Look for a CPU that is recorded as running a background task, starting at
 * prev_cpu and wrapping around. Skips CPUs the task is not allowed to run on.
 * Returns -1 if there is none.
 */
static __always_inline s32 find_bg_cpu(struct task_struct *p, s32 prev_cpu)
{
	u32 nr = nr_cpus;
	int i;

	if (nr == 0 || nr > MAX_CPUS)
		nr = MAX_CPUS;
	if (prev_cpu < 0 || (u32)prev_cpu >= nr)
		prev_cpu = 0;

	bpf_for(i, 0, MAX_CPUS) {
		s32 cpu;

		if ((u32)i >= nr)
			break;
		cpu = (prev_cpu + i) % nr;
		if (cpu_state_of(cpu) == CPU_BG &&
		    bpf_cpumask_test_cpu(cpu, p->cpus_ptr))
			return cpu;
	}
	return -1;
}

/* ---- scheduling ---- */

s32 BPF_STRUCT_OPS(ufs_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	bool is_idle = false;
	bool ts = task_tier(p) == TIER_TS;
	s32 cpu;

	/* 1 and 2: an idle CPU, previous CPU first. Same helper as step 1. */
	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (is_idle) {
		if (ts) {
			stat_inc(ST_TS_IDLE);
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, TS_SLICE_NS, 0);
		} else {
			scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
		}
		return cpu;
	}

	/*
	 * No idle CPU. A time-sensitive task prefers a CPU running background
	 * work. We only RETURN the CPU here; enqueue() inserts the task and
	 * kicks, so the insert happens before the kick.
	 */
	if (ts) {
		s32 bg_cpu = find_bg_cpu(p, prev_cpu);

		if (bg_cpu >= 0)
			return bg_cpu;
	}
	return cpu;
}

void BPF_STRUCT_OPS(ufs_enqueue, struct task_struct *p, u64 enq_flags)
{
	s32 cpu;
	u8 state;

	if (task_tier(p) != TIER_TS) {
		stat_inc(ST_BG_ENQ);
		scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
		return;
	}

	/* The CPU select_cpu() chose, or the CPU the task was running on. */
	cpu = scx_bpf_task_cpu(p);
	state = cpu_state_of(cpu);

	if (state == CPU_BG) {
		/* Express lane: go first and push the background task off. */
		stat_inc(ST_TS_PREEMPT);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | cpu, TS_SLICE_NS,
				   enq_flags | SCX_ENQ_HEAD);
		scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT);
	} else if (state == CPU_TS) {
		/* Collision: never preempt another time-sensitive task. */
		stat_inc(ST_TS_QUEUED);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | cpu, TS_SLICE_NS,
				   enq_flags);
	} else {
		/* Nothing recorded: idle, or the task just stopped here. */
		stat_inc(ST_TS_NUDGE);
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL_ON | cpu, TS_SLICE_NS,
				   enq_flags);
		scx_bpf_kick_cpu(cpu, 0);
	}
}

void BPF_STRUCT_OPS(ufs_dispatch, s32 cpu, struct task_struct *prev)
{
	scx_bpf_dsq_move_to_local(SHARED_DSQ);
}

/* Record what each CPU is running. */
void BPF_STRUCT_OPS(ufs_running, struct task_struct *p)
{
	cpu_state_set(task_tier(p) == TIER_TS ? CPU_TS : CPU_BG);
}

void BPF_STRUCT_OPS(ufs_stopping, struct task_struct *p, bool runnable)
{
	cpu_state_set(CPU_NONE);
}

/* ---- tier detection happens here (step 1) ---- */

void BPF_STRUCT_OPS(ufs_enable, struct task_struct *p)
{
	char name[NAME_LEN] = {};
	struct task_ctx *tctx;
	int class;

	class = classify(p, name);
	stat_inc(class);

	tctx = bpf_task_storage_get(&task_ctxs, p, 0, BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (tctx)
		tctx->tier = (class == CLASS_TS) ? TIER_TS : TIER_BG;

#if UFS_DEBUG
	bpf_printk("ufs: pid=%d comm=%s cgroup=%s class=%d",
		   p->pid, p->comm, name, class);
#endif
}

/* ---- per-cgroup records and queues (step 3A) ---- */

/*
 * A cgroup is being enabled in sched_ext. This is called once for every cgroup
 * that already exists when the scheduler starts, and then for each new one. It
 * is sleepable, so creating a queue here is allowed.
 */
s32 BPF_STRUCT_OPS_SLEEPABLE(ufs_cgroup_init, struct cgroup *cgrp,
			     struct scx_cgroup_init_args *args)
{
	char name[NAME_LEN] = {};
	struct cgrp_ctx *cctx;
	u64 id = BPF_CORE_READ(cgrp, kn, id);
	int class, ret;

	class = classify_name(BPF_CORE_READ(cgrp, kn, name), name);

	cctx = bpf_cgrp_storage_get(&cgrp_ctxs, cgrp, 0, BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!cctx) {
		stat_inc(ST_CGRP_ERR);
		return -12;	/* -ENOMEM */
	}

	cctx->tier = (class == CLASS_TS) ? TIER_TS : TIER_BG;
	cctx->dsq_id = id;
	cctx->has_dsq = 0;

	if (class == CLASS_BG) {
		/* Only background cgroups get their own queue. */
		ret = scx_bpf_create_dsq(id, -1);
		if (ret) {
			stat_inc(ST_CGRP_ERR);
			return ret;
		}
		cctx->has_dsq = 1;
		stat_inc(ST_CGRP_BG);
	} else if (class == CLASS_TS) {
		stat_inc(ST_CGRP_TS);
	} else {
		stat_inc(ST_CGRP_OTHER);
	}

#if UFS_DEBUG
	bpf_printk("ufs: cgroup_init id=%llu name=%s class=%d", id, name, class);
#endif
	return 0;
}

/* The cgroup is going away: destroy its queue, if it had one. */
void BPF_STRUCT_OPS(ufs_cgroup_exit, struct cgroup *cgrp)
{
	struct cgrp_ctx *cctx = bpf_cgrp_storage_get(&cgrp_ctxs, cgrp, 0, 0);

	if (cctx && cctx->has_dsq) {
		scx_bpf_destroy_dsq(cctx->dsq_id);
		cctx->has_dsq = 0;
		stat_inc(ST_CGRP_DESTROYED);
#if UFS_DEBUG
		bpf_printk("ufs: cgroup_exit id=%llu queue destroyed", cctx->dsq_id);
#endif
	}
}

s32 BPF_STRUCT_OPS_SLEEPABLE(ufs_init)
{
	return scx_bpf_create_dsq(SHARED_DSQ, -1);
}

void BPF_STRUCT_OPS(ufs_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(pgtier_ops,
	       .select_cpu	= (void *)ufs_select_cpu,
	       .enqueue		= (void *)ufs_enqueue,
	       .dispatch	= (void *)ufs_dispatch,
	       .running		= (void *)ufs_running,
	       .stopping	= (void *)ufs_stopping,
	       .enable		= (void *)ufs_enable,
	       .cgroup_init	= (void *)ufs_cgroup_init,
	       .cgroup_exit	= (void *)ufs_cgroup_exit,
	       .init		= (void *)ufs_init,
	       .exit		= (void *)ufs_exit,
	       .flags		= SCX_OPS_SWITCH_PARTIAL,
	       .name		= "pgtier");
