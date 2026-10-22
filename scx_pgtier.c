/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_pgtier loader, step 1 of 7 (tier detection).
 *
 * Loads the scheduler, keeps it attached, prints how many tasks were
 * classified into each class once a second, and reports why the scheduler
 * exited. Structure follows scx_simple.c.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_pgtier.bpf.skel.h"

/* Must match the number of entries in the "stats" map in the BPF file. */
#define NR_STATS	3

const char help_fmt[] =
"Tier-detection step of scx_pgtier (partial mode).\n"
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

/* Sum the per-CPU counters of each class. */
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

	SCX_OPS_LOAD(skel, pgtier_ops, scx_pgtier, uei);
	link = SCX_OPS_ATTACH(skel, pgtier_ops, scx_pgtier);

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 stats[NR_STATS];

		read_stats(skel, stats);
		printf("bg=%llu ts=%llu unknown=%llu\n",
		       stats[0], stats[1], stats[2]);
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
