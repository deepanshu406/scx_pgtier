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
"Usage: %s [-v] [-b] [-d CONNINFO]\n"
"\n"
"  -d CONNINFO   PostgreSQL connection string (for example\n"
"                \"host=/tmp port=5433 dbname=bench user=postgres\").\n"
"                Background backends that block other backends are treated\n"
"                as time-sensitive while they block. Use a role that has no\n"
"                pgtier.tier, and that may read pg_stat_activity.\n"
"                Without -d, nothing is boosted.\n"
"  -b            With -d: print one line per boost (when it started, how long it\n"
"                lasted, CPU used and CPU wait of the holder). Uses a heavier\n"
"                query (pg_locks), so use it for measuring only.\n"
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
 * Boost trace (-b): one record per holder that is being boosted.
 *   wait_since   when the first waiter started to wait (pg_locks.waitstart), 0 if unknown
 *   seen         when this loader first saw the holder blocking someone
 *   cpu0/delay0  /proc/PID/schedstat at that moment (CPU time, time waiting for a CPU)
 *   cpu1/delay1  the last reading while the holder was alive
 * The holder exits right after it commits, so the "end" numbers are the last
 * reading taken while it was alive (at most one poll, 20 ms, before the end).
 */
#define MAX_TRACE	64

struct btrace {
	__u32 pid;
	double wait_since, seen;
	__u64 cpu0, delay0, cpu1, delay1;
};

static bool boost_trace;
static struct btrace trace[MAX_TRACE];
static int ntrace;

/* Wall clock in seconds; pg_locks.waitstart is wall clock too. */
static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* CLOCK_MONOTONIC in ns: the clock bpf_ktime_get_ns() uses. */
static __u64 mono_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* CPU time and run-queue wait (both ns) of a process. 0 on success. */
static int read_sched(__u32 pid, __u64 *cpu, __u64 *delay)
{
	char path[64];
	unsigned long long a, b;
	FILE *f;
	int n;

	snprintf(path, sizeof(path), "/proc/%u/schedstat", pid);
	f = fopen(path, "r");
	if (!f)
		return -1;
	n = fscanf(f, "%llu %llu", &a, &b);
	fclose(f);
	if (n != 2)
		return -1;
	*cpu = a;
	*delay = b;
	return 0;
}

/*
 * Called once per poll with the holders found now (cur) and the time their first
 * waiter started waiting (since, 0 if unknown). Starts a record for a new
 * holder, refreshes the readings of the others, and prints and drops the
 * records of holders that are gone.
 */
static void trace_poll(struct scx_pgtier *skel, const __u32 *cur, const double *since, int ncur)
{
	double t = now_s();
	int i, j;

	for (j = 0; j < ntrace; ) {
		struct btrace *b = &trace[j];
		bool still = false;
		__u64 c, d;

		for (i = 0; i < ncur; i++) {
			if (cur[i] == b->pid) {
				still = true;
				break;
			}
		}
		if (still) {
			j++;
			continue;
		}
		if (read_sched(b->pid, &c, &d) == 0) {
			b->cpu1 = c;
			b->delay1 = d;
		}
		printf("boost pid %u: noticed ", b->pid);
		if (b->wait_since > 0)
			printf("%.0f ms after the waiter started waiting", (b->seen - b->wait_since) * 1e3);
		else
			printf("(waiter start unknown)");
		printf(", boost lasted %.3f s", t - b->seen);
		if (b->wait_since > 0)
			printf(", waiter waited %.3f s in total", t - b->wait_since);
		printf("\n  cpu before boost %.3f s; during boost: cpu %.3f s, waiting for a cpu %.3f s\n",
		       b->cpu0 / 1e9, (b->cpu1 - b->cpu0) / 1e9, (b->delay1 - b->delay0) / 1e9);
		{
			struct ufs_btrace bt;
			double dur = t - b->seen, other;
			const char *what[] = { "unknown", "had just woken up", "was waiting in a queue",
					       "was running on a CPU" };

			if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.btrace), &b->pid, &bt) == 0) {
				other = dur - (bt.wait_ns + bt.run_ns) / 1e9;
				if (other < 0)
					other = 0;
				printf("  scheduler view: at boost time the holder %s;\n",
				       what[bt.first_ev <= BT_EV_STOPPING ? bt.first_ev : 0]);
				if (bt.first_run_ns)
					printf("    boost set -> first ran: %.1f ms;",
					       (bt.first_run_ns - bt.set_ns) / 1e6);
				else
					printf("    boost set -> never ran;");
				printf(" waiting in our queues: %.3f s (%u runs, longest wait %.1f ms);\n",
				       bt.wait_ns / 1e9, bt.nr_run, bt.max_wait_ns / 1e6);
				printf("    on a CPU: %.3f s; neither (asleep or unaccounted): %.3f s;"
				       " enqueued %u times\n",
				       bt.run_ns / 1e9, other, bt.nr_enq);
				printf("    slice at start of a run: %.2f ms on average;"
				       " stops: %u slice ended, %u kicked off early",
				       bt.nr_run ? bt.slice_given_ns / 1e6 / bt.nr_run : 0.0,
				       bt.nr_stop_end, bt.nr_stop_early);
				if (bt.nr_stop_early)
					printf(" (%.2f ms of slice left on average)",
					       bt.early_left_ns / 1e6 / bt.nr_stop_early);
				printf(", %u went to sleep\n", bt.nr_stop_sleep);
			}
			bpf_map_delete_elem(bpf_map__fd(skel->maps.btrace), &b->pid);
		}
		fflush(stdout);
		*b = trace[--ntrace];
	}

	for (i = 0; i < ncur; i++) {
		struct btrace *b = NULL;
		__u64 c, d;

		for (j = 0; j < ntrace; j++) {
			if (trace[j].pid == cur[i]) {
				b = &trace[j];
				break;
			}
		}
		if (b) {
			if (read_sched(cur[i], &c, &d) == 0) {
				b->cpu1 = c;
				b->delay1 = d;
			}
			if (since[i] > 0 && (b->wait_since == 0 || since[i] < b->wait_since))
				b->wait_since = since[i];
		} else if (ntrace < MAX_TRACE && read_sched(cur[i], &c, &d) == 0) {
			struct ufs_btrace bt = {};

			b = &trace[ntrace++];
			b->pid = cur[i];
			b->wait_since = since[i];
			b->seen = t;
			b->cpu0 = b->cpu1 = c;
			b->delay0 = b->delay1 = d;
			/* Created before the PID goes into the boost map. */
			bt.set_ns = mono_ns();
			bpf_map_update_elem(bpf_map__fd(skel->maps.btrace), &b->pid, &bt, BPF_ANY);
		}
	}
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
	double since[MAX_BOOST];
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

	/*
	 * Without -b this is the normal query. With -b the query also returns when
	 * the first waiter started waiting (pg_locks.waitstart, PostgreSQL 14+).
	 */
	res = PQexec(conn, boost_trace ?
		     "SELECT h.holder, extract(epoch FROM min(l.waitstart))::float8 "
		     "FROM pg_locks l "
		     "CROSS JOIN LATERAL unnest(pg_blocking_pids(l.pid)) AS h(holder) "
		     "WHERE NOT l.granted GROUP BY h.holder" :
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

		if (is_bg_pid(pid)) {
			since[ncur] = (boost_trace && PQnfields(res) > 1 && !PQgetisnull(res, i, 1)) ?
				      atof(PQgetvalue(res, i, 1)) : 0;
			cur[ncur++] = pid;
		}
	}
	PQclear(res);

	if (boost_trace)
		trace_poll(skel, cur, since, ncur);

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

	while ((opt = getopt(argc, argv, "vhbd:")) != -1) {
		switch (opt) {
		case 'v':
			verbose = true;
			break;
		case 'd':
			conninfo = optarg;
			break;
		case 'b':
			boost_trace = true;
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
	skel->rodata->trace_on = boost_trace;

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
