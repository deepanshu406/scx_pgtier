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

#endif /* SCX_PGTIER_H */
