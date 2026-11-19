/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared between the BPF program (scx_pgtier.bpf.c) and the loader
 * (scx_pgtier.c): the layout of one entry of the "slots" table.
 *
 * Each background cgroup that has its own queue owns one slot. Slot 0 is
 * reserved for the shared queue, which is used by background tasks that are not
 * in a "ufs-bg-" cgroup.
 */
#ifndef SCX_PGTIER_H
#define SCX_PGTIER_H

#define UFS_MAX_SLOTS	64

/* Values of ufs_slot.state */
enum {
	UFS_SLOT_FREE = 0,
	UFS_SLOT_FILLING = 1,	/* being set up or torn down: dispatch must skip it */
	UFS_SLOT_READY = 2,
};

struct ufs_slot {
	__u64 vtime;		/* the cgroup's virtual clock */
	__u64 tvtime_now;	/* highest task clock seen running in this cgroup */
	__u64 dsq_id;		/* id of the cgroup's queue */
	__u32 weight;		/* cpu.weight, 1..10000 */
	__u32 state;		/* UFS_SLOT_* */
};

/*
 * Boost trace (loader option -b): what happens to one boosted lock holder.
 * The loader creates the entry (and sets set_ns) when it first sees the holder
 * blocking someone, reads it when the holder stops blocking, then deletes it.
 * The scheduler fills in the rest. All times are CLOCK_MONOTONIC nanoseconds.
 */
enum {
	BT_EV_NONE = 0,
	BT_EV_ENQUEUE = 1,	/* first event seen was enqueue: it had just woken up */
	BT_EV_RUNNING = 2,	/* first event was running: it was waiting in a queue */
	BT_EV_STOPPING = 3,	/* first event was stopping: it was on a CPU */
};

struct ufs_btrace {
	__u64 set_ns;		/* loader: boost decided */
	__u64 first_run_ns;	/* first time it started running after set_ns, 0 = not yet */
	__u64 enq_ns;		/* when it was last enqueued; 0 = not waiting */
	__u64 run_start_ns;	/* when it last started running; 0 = not running */
	__u64 wait_ns;		/* total time from enqueue (or set_ns) to running */
	__u64 max_wait_ns;	/* longest single wait */
	__u64 run_ns;		/* total time on a CPU since set_ns */
	__u64 slice_given_ns;	/* sum of the slice it had at the start of each run */
	__u64 early_left_ns;	/* sum of the slice left at each "kicked off early" stop */
	__u32 nr_enq;		/* times enqueued since set_ns */
	__u32 nr_run;		/* times started running since set_ns */
	__u32 first_ev;		/* BT_EV_* */
	__u32 nr_stop_early;	/* stops with the task still runnable and more than 1 ms of slice left */
	__u32 nr_stop_end;	/* stops with the task still runnable and (almost) no slice left */
	__u32 nr_stop_sleep;	/* stops because the task went to sleep */
};

#endif /* SCX_PGTIER_H */
