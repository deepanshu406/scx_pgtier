/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_pgtier: step 3C of 7, cgroup virtual clocks and weighted dispatch, on
 * top of 3B (cgroup weights), 3A (per-cgroup records and queues), step 2 (the
 * express lane) and step 1 (tier detection).
 *
 * Step 3C (new):
 *   - A table of UFS_MAX_SLOTS "slots". Slot 0 stands for the shared queue.
 *     Every "ufs-bg-*" cgroup gets one slot in cgroup_init. A slot holds the
 *     cgroup's virtual clock (vtime), weight and queue id.
 *   - Background tasks are put in their cgroup's queue (enqueue), ordered by
 *     their own task clock. A background task whose cgroup has no queue uses
 *     the shared queue (slot 0).
 *   - Task clock: advanced in stopping() by the time the task really ran,
 *     divided by its weight (as scx_simple does).
 *   - dispatch(): scan the slots, pick the READY slot with the lowest cgroup
 *     clock among those with waiting tasks, take its first task, and charge
 *     that cgroup SCX_SLICE_DFL * 100 / weight. If the queue was emptied by
 *     another CPU in the meantime, try again (a few times).
 *   - A new cgroup's clock starts at cg_vtime_now (the clock of the cgroup most
 *     recently picked), so it does not start far behind the others.
 * Not yet: the clamp for a cgroup that was idle (step 3D), the rbtree instead
 * of the scan, unknown cgroups as their own slots.
 *
 * Known limits:
 *   - A task's slot is read once, in enable(). Move the task into its cgroup
 *     BEFORE switching it to SCHED_EXT. A later move is not noticed.
 *   - Tasks that sit in the shared or a cgroup queue while their cgroup is
 *     removed are not handled.
 *
 * Step 3B: every cgroup record stores the cgroup's cpu.weight (starting value
 * from the cgroup_init arguments, updates from cgroup_set_weight, clamped to
 * 1..10000). Assumption: our "ufs-" cgroups are siblings under one parent.
 *
 * Step 3A: cgroup_init reads the cgroup NAME, stores a record on the cgroup and,
 * for a "ufs-bg-*" cgroup, creates its own queue (DSQ) with the cgroup's id.
 * cgroup_exit destroys it. The cpu controller must be enabled for a cgroup to
 * get its own cgroup_init call (echo +cpu > /sys/fs/cgroup/cgroup.subtree_control).
 *
 * Step 2: time-sensitive tasks get a CPU quickly.
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
 *
 * Step 1: when a task joins sched_ext (enable), read the NAME of its cgroup.
 * "ufs-ts-*" -> time-sensitive tier, "ufs-bg-*" -> background tier, anything
 * else -> counted as unknown and treated as background.
 *
 * Run it in PARTIAL mode: only tasks switched to SCHED_EXT are ours.
 *
 * The scheduling callbacks follow tools/sched_ext/scx_simple.bpf.c
 * (Copyright (c) 2022 Meta Platforms, Inc. and affiliates;
 *  Copyright (c) 2022 Tejun Heo <tj@kernel.org>;
 *  Copyright (c) 2022 David Vernet <dvernet@meta.com>).
 */
#include <scx/common.bpf.h>
#include "scx_pgtier.h"

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

/* Id of the shared queue. It belongs to slot 0. */
#define SHARED_DSQ	0

/* Set to 0 to silence the trace_pipe output. */
#define UFS_DEBUG	1

/* Slice for time-sensitive tasks: short, so a task queued behind one waits little. */
#define TS_SLICE_NS	(2 * 1000 * 1000ULL)

/* How many times dispatch() tries again after losing a race for a queue. */
#define UFS_MAX_PICK_TRIES	4

#define MAX_CPUS	1024

/* Number of possible CPUs; the loader fills this in before loading. */
const volatile u32 nr_cpus = MAX_CPUS;

/* Clock of the cgroup picked most recently. A new cgroup's clock starts here. */
u64 cg_vtime_now;

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
	ST_BG_ENQ,			/* bg task put on the shared queue (slot 0) */
	ST_CGRP_BG,			/* "ufs-bg-*" cgroup seen: record made, queue created */
	ST_CGRP_TS,			/* "ufs-ts-*" cgroup seen: record made, no queue */
	ST_CGRP_OTHER,			/* any other cgroup seen: record made, no queue */
	ST_CGRP_DESTROYED,		/* a cgroup queue was destroyed in cgroup_exit */
	ST_CGRP_ERR,			/* creating a record or queue failed */
	ST_CGRP_WSET,			/* cgroup_set_weight was called */
	ST_BG_CGQ,			/* bg task put on a cgroup queue */
	ST_DISP_OK,			/* dispatch moved a task to a CPU's local queue */
	ST_DISP_RETRY,			/* dispatch lost a race for a queue and tried again */
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
	u8  tier;
	u32 slot;	/* index into the slots table; 0 = shared queue */
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
	u8  has_dsq;	/* 1 if this cgroup owns a queue and a slot */
	u32 weight;	/* cpu.weight, clamped to 1..10000 */
	u32 slot;	/* index into the slots table (0 if has_dsq is 0) */
	u64 dsq_id;	/* the queue's id: the cgroup's own id */
};

#define WEIGHT_MIN	1
#define WEIGHT_MAX	10000

struct {
	__uint(type, BPF_MAP_TYPE_CGRP_STORAGE);
	__uint(map_flags, BPF_F_NO_PREALLOC);
	__type(key, int);
	__type(value, struct cgrp_ctx);
} cgrp_ctxs SEC(".maps");

/*
 * The slots table. dispatch() loops over it to find the cgroup with the lowest
 * clock. Entry 0 is the shared queue; entries 1.. belong to ufs-bg-* cgroups.
 */
struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, UFS_MAX_SLOTS);
	__type(key, u32);
	__type(value, struct ufs_slot);
} slots SEC(".maps");

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

static __always_inline struct ufs_slot *slot_get(u32 idx)
{
	return bpf_map_lookup_elem(&slots, &idx);
}

static __always_inline u8 task_tier(struct task_struct *p)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);

	return tctx ? tctx->tier : TIER_BG;
}

/* Keep a weight inside the range cpu.weight allows. */
static __always_inline u32 clamp_weight(u32 w)
{
	if (w < WEIGHT_MIN)
		return WEIGHT_MIN;
	if (w > WEIGHT_MAX)
		return WEIGHT_MAX;
	return w;
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

/* ---- the slots table ---- */

/* Claim a free slot (1..UFS_MAX_SLOTS-1). Returns its index, or 0 if none is free. */
static __always_inline u32 alloc_slot(void)
{
	int i;

	bpf_for(i, 1, UFS_MAX_SLOTS) {
		struct ufs_slot *s = slot_get(i);

		if (s && __sync_val_compare_and_swap(&s->state, UFS_SLOT_FREE,
						     UFS_SLOT_FILLING) == UFS_SLOT_FREE)
			return i;
	}
	return 0;
}

static __always_inline void free_slot(struct ufs_slot *s)
{
	s->vtime = 0;
	s->tvtime_now = 0;
	s->dsq_id = 0;
	s->weight = 0;
	s->state = UFS_SLOT_FREE;
}

/*
 * The READY slot with the lowest cgroup clock among those with waiting tasks.
 * Returns its index, or -1 if every queue is empty.
 */
static __always_inline int pick_slot(void)
{
	u64 best_vtime = 0;
	int best = -1;
	int i;

	bpf_for(i, 0, UFS_MAX_SLOTS) {
		struct ufs_slot *s = slot_get(i);

		if (!s || s->state != UFS_SLOT_READY)
			continue;
		if (scx_bpf_dsq_nr_queued(s->dsq_id) <= 0)
			continue;
		if (best < 0 || time_before(s->vtime, best_vtime)) {
			best = i;
			best_vtime = s->vtime;
		}
	}
	return best;
}

/* Put a background task on its cgroup's queue, or on the shared queue. */
static __always_inline void enqueue_bg(struct task_struct *p, u64 enq_flags)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);
	u32 slot = tctx ? tctx->slot : 0;
	struct ufs_slot *s = slot_get(slot);
	u64 vtime = p->scx.dsq_vtime;

	if (!s || s->state != UFS_SLOT_READY) {
		/* The cgroup's queue is gone: use the shared queue. */
		slot = 0;
		s = slot_get(0);
		if (!s) {
			scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
			return;
		}
	}

	/* A task that has been idle may only be one slice behind the others. */
	if (time_before(vtime, s->tvtime_now - SCX_SLICE_DFL))
		vtime = s->tvtime_now - SCX_SLICE_DFL;

	stat_inc(slot ? ST_BG_CGQ : ST_BG_ENQ);
	scx_bpf_dsq_insert_vtime(p, s->dsq_id, SCX_SLICE_DFL, vtime, enq_flags);
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
		enqueue_bg(p, enq_flags);
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

/*
 * This CPU's local queue is empty: pick the cgroup with the lowest clock, take
 * its first task, and charge the cgroup for one slice.
 */
void BPF_STRUCT_OPS(ufs_dispatch, s32 cpu, struct task_struct *prev)
{
	int tries;

	bpf_for(tries, 0, UFS_MAX_PICK_TRIES) {
		int idx = pick_slot();
		struct ufs_slot *s;
		u32 weight;

		if (idx < 0)
			return;

		s = slot_get(idx);
		if (!s || s->state != UFS_SLOT_READY)
			continue;

		if (scx_bpf_dsq_move_to_local(s->dsq_id)) {
			/* Remember where the clocks are now (racy, tolerated). */
			if (time_before(cg_vtime_now, s->vtime))
				cg_vtime_now = s->vtime;

			/* Charge: slice * 100 / weight. A heavier cgroup pays less. */
			weight = s->weight ? s->weight : 100;
			u64 charge = SCX_SLICE_DFL * 100 / weight;
			u64 floor  = cg_vtime_now - charge;

			if (time_before(s->vtime, floor))
				s->vtime = floor; /* racy, tolerated */
			__sync_fetch_and_add(&s->vtime, SCX_SLICE_DFL * 100 / weight);

			stat_inc(ST_DISP_OK);
			return;
		}
		/* Another CPU emptied the queue first: look again. */
		stat_inc(ST_DISP_RETRY);
	}
}

/* Record what each CPU is running, and how far the cgroup's tasks have got. */
void BPF_STRUCT_OPS(ufs_running, struct task_struct *p)
{
	struct task_ctx *tctx = bpf_task_storage_get(&task_ctxs, p, 0, 0);
	u8 tier = tctx ? tctx->tier : TIER_BG;

	cpu_state_set(tier == TIER_TS ? CPU_TS : CPU_BG);

	if (tier == TIER_BG) {
		struct ufs_slot *s = slot_get(tctx ? tctx->slot : 0);

		if (s && time_before(s->tvtime_now, p->scx.dsq_vtime))
			s->tvtime_now = p->scx.dsq_vtime;	/* racy, tolerated */
	}
}

void BPF_STRUCT_OPS(ufs_stopping, struct task_struct *p, bool runnable)
{
	cpu_state_set(CPU_NONE);

	/* Task clock: time actually used, divided by the task's weight. */
	if (task_tier(p) == TIER_BG)
		p->scx.dsq_vtime += (SCX_SLICE_DFL - p->scx.slice) * 100 / p->scx.weight;
}

/* ---- a task joins sched_ext: tier detection (step 1) and its queue (3C) ---- */

void BPF_STRUCT_OPS(ufs_enable, struct task_struct *p)
{
	char name[NAME_LEN] = {};
	struct task_ctx *tctx;
	struct cgroup *cgrp;
	struct ufs_slot *s;
	u32 slot = 0;
	int class;

	class = classify(p, name);
	stat_inc(class);

	tctx = bpf_task_storage_get(&task_ctxs, p, 0, BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!tctx)
		return;
	tctx->tier = (class == CLASS_TS) ? TIER_TS : TIER_BG;

	/*
	 * Which queue? The one of the task's cgroup, if that cgroup has one.
	 * ops.enable() is a place where scx_bpf_task_cgroup() may be asked about
	 * the task. It returns a reference that must be released.
	 */
	cgrp = scx_bpf_task_cgroup(p);
	if (cgrp) {
		struct cgrp_ctx *cctx = bpf_cgrp_storage_get(&cgrp_ctxs, cgrp, 0, 0);

		if (cctx && cctx->has_dsq)
			slot = cctx->slot;
		bpf_cgroup_release(cgrp);
	}
	tctx->slot = slot;

	/* Start the task's clock where its cgroup's tasks are now. */
	s = slot_get(slot);
	p->scx.dsq_vtime = s ? s->tvtime_now : 0;

#if UFS_DEBUG
	bpf_printk("ufs: pid=%d comm=%s cgroup=%s class=%d slot=%u",
		   p->pid, p->comm, name, class, slot);
#endif
}

/* ---- per-cgroup records, queues and slots (steps 3A to 3C) ---- */

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
	u32 slot = 0;
	int class, ret;

	class = classify_name(BPF_CORE_READ(cgrp, kn, name), name);

	cctx = bpf_cgrp_storage_get(&cgrp_ctxs, cgrp, 0, BPF_LOCAL_STORAGE_GET_F_CREATE);
	if (!cctx) {
		stat_inc(ST_CGRP_ERR);
		return -12;	/* -ENOMEM */
	}

	cctx->tier = (class == CLASS_TS) ? TIER_TS : TIER_BG;
	cctx->weight = clamp_weight(args->weight);
	cctx->dsq_id = id;
	cctx->has_dsq = 0;
	cctx->slot = 0;

	if (class == CLASS_BG) {
		/*
		 * Only background cgroups get a queue and a slot. If we run out
		 * of slots or the queue cannot be created, the cgroup still
		 * works: its tasks use the shared queue.
		 */
		slot = alloc_slot();
		if (!slot) {
			stat_inc(ST_CGRP_ERR);
		} else {
			ret = scx_bpf_create_dsq(id, -1);
			if (ret) {
				struct ufs_slot *s = slot_get(slot);

				if (s)
					free_slot(s);
				slot = 0;
				stat_inc(ST_CGRP_ERR);
			} else {
				struct ufs_slot *s = slot_get(slot);

				if (s) {
					s->dsq_id = id;
					s->weight = cctx->weight;
					s->vtime = cg_vtime_now;
					s->tvtime_now = 0;
					s->state = UFS_SLOT_READY;
					cctx->has_dsq = 1;
					cctx->slot = slot;
				}
				stat_inc(ST_CGRP_BG);
			}
		}
	} else if (class == CLASS_TS) {
		stat_inc(ST_CGRP_TS);
	} else {
		stat_inc(ST_CGRP_OTHER);
	}

#if UFS_DEBUG
	bpf_printk("ufs: cgroup_init id=%llu name=%s class=%d weight=%u slot=%u",
		   id, name, class, cctx->weight, cctx->slot);
#endif
	return 0;
}

/* Someone wrote to this cgroup's cpu.weight: store the new value. */
void BPF_STRUCT_OPS(ufs_cgroup_set_weight, struct cgroup *cgrp, u32 weight)
{
	struct cgrp_ctx *cctx = bpf_cgrp_storage_get(&cgrp_ctxs, cgrp, 0, 0);

	stat_inc(ST_CGRP_WSET);
	if (!cctx)
		return;

	cctx->weight = clamp_weight(weight);
	if (cctx->has_dsq) {
		struct ufs_slot *s = slot_get(cctx->slot);

		if (s)
			s->weight = cctx->weight;
	}
#if UFS_DEBUG
	bpf_printk("ufs: cgroup_set_weight id=%llu raw=%u stored=%u",
		   cctx->dsq_id, weight, cctx->weight);
#endif
}

/* The cgroup is going away: destroy its queue and free its slot. */
void BPF_STRUCT_OPS(ufs_cgroup_exit, struct cgroup *cgrp)
{
	struct cgrp_ctx *cctx = bpf_cgrp_storage_get(&cgrp_ctxs, cgrp, 0, 0);

	if (cctx && cctx->has_dsq) {
		struct ufs_slot *s = slot_get(cctx->slot);

		/* Hide the slot from dispatch before the queue disappears. */
		if (s)
			s->state = UFS_SLOT_FILLING;

		scx_bpf_destroy_dsq(cctx->dsq_id);

		if (s)
			free_slot(s);
		cctx->has_dsq = 0;
		stat_inc(ST_CGRP_DESTROYED);
#if UFS_DEBUG
		bpf_printk("ufs: cgroup_exit id=%llu queue destroyed", cctx->dsq_id);
#endif
	}
}

s32 BPF_STRUCT_OPS_SLEEPABLE(ufs_init)
{
	struct ufs_slot *s;
	int ret;

	/* Slot 0 is the shared queue, used by background tasks without a queue. */
	ret = scx_bpf_create_dsq(SHARED_DSQ, -1);
	if (ret)
		return ret;

	s = slot_get(0);
	if (s) {
		s->dsq_id = SHARED_DSQ;
		s->weight = 100;
		s->vtime = 0;
		s->tvtime_now = 0;
		s->state = UFS_SLOT_READY;
	}
	return 0;
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
	       .cgroup_set_weight = (void *)ufs_cgroup_set_weight,
	       .init		= (void *)ufs_init,
	       .exit		= (void *)ufs_exit,
	       .flags		= SCX_OPS_SWITCH_PARTIAL,
	       .name		= "pgtier");
