/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_pgtier loader, step 3C of 7 (tier detection + express lane + per-cgroup
 * queues + cgroup weights + weighted cgroup clocks).
 *
 * Loads the scheduler, keeps it attached, prints the counters once a second,
 * and reports why the scheduler exited. Structure follows scx_simple.c
 * (Copyright (c) 2022 Meta Platforms, Inc. and affiliates;
 *  Copyright (c) 2022 Tejun Heo <tj@kernel.org>;
 *  Copyright (c) 2022 David Vernet <dvernet@meta.com>).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_pgtier.h"
#include "scx_pgtier.bpf.skel.h"

/*
 * Counter names, in the order of the "stats" map in the BPF file:
 *   0 bg, 1 ts, 2 unknown          tasks seen at enable, by class
 *   3 ts_idle                      ts task placed on an idle CPU
 *   4 ts_preempt                   ts task jumped the queue and kicked a bg task
 *   5 ts_queued                    ts task queued behind a running ts task
 *   6 ts_nudge                     ts task queued on a CPU with nothing recorded
 *   7 bg_enq                       bg task put on the shared queue
 *   8 cg_back                      "ufs-bg-*" cgroup seen: record made, queue created
 *   9 cg_time                      "ufs-ts-*" cgroup seen: record made
 *  10 cg_other                     any other cgroup seen: record made
 *  11 cg_destroyed                 a cgroup queue was destroyed
 *  12 cg_err                       creating a record or queue failed
 *  13 cg_wset                      cgroup_set_weight was called
 *  14 bg_cgq                       bg task put on a cgroup queue
 *  15 disp_ok                      dispatch moved a task to a local queue
 *  16 disp_retry                   dispatch lost a race and tried again
 * (The cg_ names avoid the substrings "bg=" and "ts=", which the step 1 test
 * looks for in the output line.)
 */
#define NR_STATS	17

const char help_fmt[] =
"scx_pgtier, step 3C: tiers, express lane, weighted cgroup queues (partial mode).\n"
"\n"
"Only tasks switched to SCHED_EXT are scheduled by this scheduler.\n"
"cgroup names: ufs-ts-* = time-sensitive, ufs-bg-* = background.\n"
"\n"
"Usage: %s [-v]\n"
"\n"
"  -v            Print libbpf debug messages\n"
"  -h            Display this help and exit\n";

static bool verbose;
static volatile int exit_req;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int sig)
{
	exit_req = 1;
}

/* Sum the per-CPU counters of each stat. */
static void read_stats(struct scx_pgtier *skel, __u64 *stats)
{
	int nr_cpus = libbpf_num_possible_cpus();
	assert(nr_cpus > 0);
	__u64 cnts[NR_STATS][nr_cpus];
	__u32 idx;

	memset(stats, 0, sizeof(stats[0]) * NR_STATS);

	for (idx = 0; idx < NR_STATS; idx++) {
		int ret, cpu;

		ret = bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats),
					  &idx, cnts[idx]);
		if (ret < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			stats[idx] += cnts[idx][cpu];
	}
}

/* One line per slot in use. These lines must not start with "bg=". */
static void print_slots(struct scx_pgtier *skel)
{
	int fd = bpf_map__fd(skel->maps.slots);
	__u32 i;

	for (i = 0; i < UFS_MAX_SLOTS; i++) {
		struct ufs_slot s;

		if (bpf_map_lookup_elem(fd, &i, &s) < 0 || s.state != UFS_SLOT_READY)
			continue;
		printf("  slot %u: id=%llu weight=%u vtime=%llu\n",
		       i, (unsigned long long)s.dsq_id, s.weight,
		       (unsigned long long)s.vtime);
	}
}

int main(int argc, char **argv)
{
	struct scx_pgtier *skel;
	struct bpf_link *link;
	int opt;
	__u64 ecode;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	skel = SCX_OPS_OPEN(pgtier_ops, scx_pgtier);

	while ((opt = getopt(argc, argv, "vh")) != -1) {
		switch (opt) {
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	/* The stats map and this file must agree on the number of counters. */
	if (bpf_map__max_entries(skel->maps.stats) != NR_STATS) {
		fprintf(stderr, "stats map has %u entries, loader expects %d\n",
			bpf_map__max_entries(skel->maps.stats), NR_STATS);
		return 1;
	}

	/* Tell the BPF side how many CPUs to scan. */
	skel->rodata->nr_cpus = libbpf_num_possible_cpus();

	SCX_OPS_LOAD(skel, pgtier_ops, scx_pgtier, uei);
	link = SCX_OPS_ATTACH(skel, pgtier_ops, scx_pgtier);

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 s[NR_STATS];

		read_stats(skel, s);
		/* Keep the "bg= ts= unknown=" prefix: the step 1 test parses it. */
		printf("bg=%llu ts=%llu unknown=%llu "
		       "ts_idle=%llu ts_preempt=%llu ts_queued=%llu ts_nudge=%llu "
		       "bg_enq=%llu cg_back=%llu cg_time=%llu cg_other=%llu "
		       "cg_destroyed=%llu cg_err=%llu cg_wset=%llu "
		       "bg_cgq=%llu disp_ok=%llu disp_retry=%llu\n",
		       s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
		       s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16]);
		print_slots(skel);
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_pgtier__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
