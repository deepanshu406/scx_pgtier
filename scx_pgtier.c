/* SPDX-License-Identifier: GPL-2.0 */
/*
 * scx_pgtier loader, step 3C of 7 (tier detection + express lane + per-cgroup
 * queues + cgroup weights + weighted cgroup clocks), plus aging and lock-holder
 * boost.
 *
 * Loads the scheduler, keeps it attached, prints the counters once a second,
 * and reports why the scheduler exited. With -d CONNINFO it also asks
 * PostgreSQL every 20 ms which backends block other backends (row locks) and
 * puts their PIDs in the "boost" map, so the scheduler treats them as
 * time-sensitive while they block someone.
 *
 * Structure follows scx_simple.c
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
#include <time.h>
#include <bpf/bpf.h>
#include <libpq-fe.h>
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
 *  17 starved                      ts enqueues made while bg tasks were starved
 * (The cg_ names avoid the substrings "bg=" and "ts=", which the step 1 test
 * looks for in the output line.)
 */
#define NR_STATS	18

/* The "boost" map in the BPF file holds at most this many PIDs. */
#define MAX_BOOST	1024

/* One poll round is this long; the counters are printed every PRINT_EVERY rounds. */
#define POLL_US		20000
#define PRINT_EVERY	50

const char help_fmt[] =
"scx_pgtier, step 3C: tiers, express lane, weighted cgroup queues (partial mode).\n"
"\n"
"Only tasks switched to SCHED_EXT are scheduled by this scheduler.\n"
"cgroup names: ufs-ts-* = time-sensitive, ufs-bg-* = background.\n"
"\n"
"Usage: %s [-v] [-d CONNINFO]\n"
"\n"
"  -d CONNINFO   PostgreSQL connection string (for example\n"
"                \"host=/tmp port=5433 dbname=bench user=postgres\").\n"
"                Background backends that block other backends are treated\n"
"                as time-sensitive while they block. Use a role that has no\n"
"                pgtier.tier, and that may read pg_stat_activity.\n"
"                Without -d, nothing is boosted.\n"
"  -v            Print libbpf debug messages\n"
"  -h            Display this help and exit\n";

static bool verbose;
static volatile int exit_req;
static const char *conninfo;
static PGconn *conn;

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

/* PIDs currently in the "boost" map, as this file last wrote it. */
static __u32 boosted[MAX_BOOST];
static int nboosted;

/* Take every PID out of the "boost" map and tell the scheduler there are none. */
static void boost_clear(struct scx_pgtier *skel)
{
	int i;

	skel->bss->boost_active = 0;
	for (i = 0; i < nboosted; i++)
		bpf_map__delete_elem(skel->maps.boost, &boosted[i],
				     sizeof(boosted[i]), 0);
	nboosted = 0;
}

/*
 * True if the process is in a "ufs-bg-*" cgroup, that is a background backend.
 * Only those are boosted: a time-sensitive backend that blocks another
 * time-sensitive backend (pgbench clients updating the same row) is already in
 * the right tier, and boosting it would only keep the scheduler in lock mode.
 */
static bool is_bg_pid(__u32 pid)
{
	char path[64], line[512];
	bool bg = false;
	FILE *f;

	snprintf(path, sizeof(path), "/proc/%u/cgroup", pid);
	f = fopen(path, "r");
	if (!f)
		return false;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "ufs-bg-")) {
			bg = true;
			break;
		}
	}
	fclose(f);
	return bg;
}

/*
 * Ask PostgreSQL which backends block other backends, and make the "boost" map
 * match (background backends only). A failure here must never stop the scheduler: on any error the map is
 * emptied (so no stale PID stays boosted) and the next try is a second later.
 */
static void boost_poll(struct scx_pgtier *skel)
{
	static time_t last_try;
	static bool warned;
	__u32 cur[MAX_BOOST];
	int ncur = 0, rows, i, j;
	__u8 one = 1;
	PGresult *res;

	if (!conninfo)
		return;

	if (!conn || PQstatus(conn) != CONNECTION_OK) {
		time_t now = time(NULL);

		if (now == last_try)
			return;
		last_try = now;
		boost_clear(skel);
		if (conn)
			PQfinish(conn);
		conn = PQconnectdb(conninfo);
		if (PQstatus(conn) != CONNECTION_OK) {
			if (!warned)
				fprintf(stderr, "boost: cannot connect to PostgreSQL, "
					"running without boost: %s", PQerrorMessage(conn));
			warned = true;
			PQfinish(conn);
			conn = NULL;
			return;
		}
		warned = false;
		fprintf(stderr, "boost: connected to PostgreSQL\n");
	}

	res = PQexec(conn,
		     "SELECT DISTINCT unnest(pg_blocking_pids(pid)) "
		     "FROM pg_stat_activity WHERE wait_event_type = 'Lock'");
	if (PQresultStatus(res) != PGRES_TUPLES_OK) {
		fprintf(stderr, "boost: query failed: %s", PQerrorMessage(conn));
		PQclear(res);
		boost_clear(skel);
		PQfinish(conn);
		conn = NULL;
		return;
	}

	rows = PQntuples(res);
	for (i = 0; i < rows && ncur < MAX_BOOST; i++) {
		__u32 pid = (__u32)atoi(PQgetvalue(res, i, 0));

		if (is_bg_pid(pid))
			cur[ncur++] = pid;
	}
	PQclear(res);

	/* Add the new PIDs first, then drop the ones that are no longer blocking. */
	for (i = 0; i < ncur; i++)
		bpf_map__update_elem(skel->maps.boost, &cur[i], sizeof(cur[i]),
				     &one, sizeof(one), BPF_ANY);
	for (i = 0; i < nboosted; i++) {
		bool still = false;

		for (j = 0; j < ncur; j++) {
			if (cur[j] == boosted[i]) {
				still = true;
				break;
			}
		}
		if (!still)
			bpf_map__delete_elem(skel->maps.boost, &boosted[i],
					     sizeof(boosted[i]), 0);
	}
	memcpy(boosted, cur, sizeof(cur[0]) * ncur);
	nboosted = ncur;
	skel->bss->boost_active = ncur;
}

int main(int argc, char **argv)
{
	struct scx_pgtier *skel;
	struct bpf_link *link;
	int opt;
	__u64 ecode;
	unsigned long round;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	skel = SCX_OPS_OPEN(pgtier_ops, scx_pgtier);

	while ((opt = getopt(argc, argv, "vhd:")) != -1) {
		switch (opt) {
		case 'v':
			verbose = true;
			break;
		case 'd':
			conninfo = optarg;
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

	nboosted = 0;
	round = 0;
	while (!exit_req && !UEI_EXITED(skel, uei)) {
		boost_poll(skel);

		if (round++ % PRINT_EVERY == 0) {
			__u64 s[NR_STATS];

			read_stats(skel, s);
			/* Keep the "bg= ts= unknown=" prefix: the step 1 test parses it. */
			printf("bg=%llu ts=%llu unknown=%llu "
			       "ts_idle=%llu ts_preempt=%llu ts_queued=%llu ts_nudge=%llu "
			       "bg_enq=%llu cg_back=%llu cg_time=%llu cg_other=%llu "
			       "cg_destroyed=%llu cg_err=%llu cg_wset=%llu "
			       "bg_cgq=%llu disp_ok=%llu disp_retry=%llu starved=%llu\n",
			       s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
			       s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16],
			       s[17]);
			print_slots(skel);
			fflush(stdout);
		}
		usleep(POLL_US);
	}

	if (conn) {
		PQfinish(conn);
		conn = NULL;
	}
	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_pgtier__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
