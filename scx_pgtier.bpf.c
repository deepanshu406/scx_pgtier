/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_pgtier: step 1 of 7, tier detection only.
 *
 * What it does:
 *   - When a task joins sched_ext (enable), read the NAME of its cgroup.
 *   - "ufs-ts-*" -> time-sensitive tier, "ufs-bg-*" -> background tier,
 *     anything else -> counted as unknown and treated as background.
 *   - Remember the tier per task (task storage) and count the results.
 *
 * What it does NOT do yet:
 *   - No express lane, no cgroup queues, no vtime. Scheduling is plain FIFO
 *     (same as scx_simple) so the machine keeps working.
 *   - It does not notice a task that moves to another cgroup later.
 *
 * Run it in PARTIAL mode: only tasks switched to SCHED_EXT are ours.
 */
#include <scx/common.bpf.h>

char _license[] SEC("license") = "GPL";

UEI_DEFINE(uei);

#define SHARED_DSQ	0

/* Set to 0 to silence the trace_pipe output. */
#define UFS_DEBUG	1

/* Tier stored on each task. */
enum ufs_tier {
	TIER_BG = 0,
	TIER_TS = 1,
};

/* What the name check found. Used as the index into the stats map. */
enum ufs_class {
	CLASS_BG = 0,		/* name starts with "ufs-bg-" */
	CLASS_TS = 1,		/* name starts with "ufs-ts-" */
	CLASS_UNKNOWN = 2,	/* any other name, or the name could not be read */
	NR_CLASSES,
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

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, NR_CLASSES);
} stats SEC(".maps");

static void stat_inc(u32 idx)
{
	u64 *cnt = bpf_map_lookup_elem(&stats, &idx);

	if (cnt)
		(*cnt)++;
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
 * Read the name of the task's cgroup (unified hierarchy) and classify it.
 * Reads p->cgroups->dfl_cgrp->kn->name with CO-RE. This needs no kfunc and no
 * reference counting.
 */
static __always_inline int classify(struct task_struct *p, char *name)
{
	const char *namep;

	namep = BPF_CORE_READ(p, cgroups, dfl_cgrp, kn, name);
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

/* ---- scheduling: plain FIFO, copied from the scx_simple pattern ---- */

s32 BPF_STRUCT_OPS(ufs_select_cpu, struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	bool is_idle = false;
	s32 cpu;

	cpu = scx_bpf_select_cpu_dfl(p, prev_cpu, wake_flags, &is_idle);
	if (is_idle)
		scx_bpf_dsq_insert(p, SCX_DSQ_LOCAL, SCX_SLICE_DFL, 0);
	return cpu;
}

void BPF_STRUCT_OPS(ufs_enqueue, struct task_struct *p, u64 enq_flags)
{
	scx_bpf_dsq_insert(p, SHARED_DSQ, SCX_SLICE_DFL, enq_flags);
}

void BPF_STRUCT_OPS(ufs_dispatch, s32 cpu, struct task_struct *prev)
{
	scx_bpf_dsq_move_to_local(SHARED_DSQ);
}

/* ---- tier detection happens here ---- */

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
	       .enable		= (void *)ufs_enable,
	       .init		= (void *)ufs_init,
	       .exit		= (void *)ufs_exit,
	       .flags		= SCX_OPS_SWITCH_PARTIAL,
	       .name		= "pgtier");
