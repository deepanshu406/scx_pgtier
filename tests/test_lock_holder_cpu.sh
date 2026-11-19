#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# How much CPU does the lock holder have when the waiter arrives, and how much
# does it use while the waiter waits? Same setup as tests/test_lock_holder.sh
# (a background session holds a row lock and needs about 1.3 s of CPU, LOAD
# time-sensitive sessions fill all CPUs, a time-sensitive transaction then
# updates the locked row), but it also works with the default scheduler.
#
# The holder's /proc/PID/schedstat (CPU time, time waiting for a CPU) is read
# every 10 ms by a tiny sampler that runs at real-time priority, so the load
# cannot starve it, and it starts no other process except "sleep".
#
# Per run, one line:
#   before = the holder's CPU time when the waiter starts to wait
#   during = the CPU time it gets while the waiter waits (and its share of the wait)
#   rq     = time it spends in the run queue (ready, no CPU) during the wait
#   total  = before + during (the last reading before the holder exits)
#
# Run as root inside the VM prepared by scripts/vm_setup.sh, from the repo root:
#
#     tests/test_lock_holder_cpu.sh quick    # one pair (ours, then EEVDF)
#     tests/test_lock_holder_cpu.sh 5        # warm-up pair, then 5 pairs
#
# Settings (environment): LOAD (16), HEAD_START (0.3 s), SCX_DB.
# Do not add -b to the scheduler arguments for this test.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../scripts/bench_funcs.sh
source "$DIR/scripts/bench_funcs.sh"

LOAD="${LOAD:-16}"
HEAD_START="${HEAD_START:-0.3}"
export SCX_DB="${SCX_DB:-host=$SOCK port=$PORT dbname=$DB user=$ADMIN}"
_P="-X -q -h $SOCK -p $PORT $DB"

_lh_setup() {
	# shellcheck disable=SC2086
	psql $_P -U "$ADMIN" <<-SQL
		DROP TABLE IF EXISTS lock_hot;
		CREATE TABLE lock_hot (id int PRIMARY KEY, v int);
		INSERT INTO lock_hot VALUES (1, 0);
		GRANT ALL ON lock_hot TO oltp_app, report_user;
	SQL
}

_lh_clean() {
	# shellcheck disable=SC2086
	psql $_P -U "$ADMIN" -Atc "select count(pg_terminate_backend(pid)) from pg_stat_activity
		where usename in ('oltp_app','report_user') and pid <> pg_backend_pid()" >/dev/null
	sleep 2
}

# The sampler: one line "time cpu_ns runqueue_wait_ns" every 10 ms, until the
# process is gone. $1 = PID, $2 = output file. "read" is a shell builtin.
_SAMPLER='
export LC_ALL=C
pid=$1; out=$2
while :; do
	read -r c d _ < "/proc/$pid/schedstat" 2>/dev/null || break
	printf "%s %s %s\n" "$EPOCHREALTIME" "$c" "$d" >> "$out"
	sleep 0.01
done'

# cpurun LABEL: one run. Prints one line.
cpurun() {
	local n=0 i t0 t1 hpid spid out
	out=$(mktemp /tmp/lh_cpu.XXXXXX)
	_lh_clean
	# shellcheck disable=SC2086
	( timeout 90 psql $_P -U report_user -c "BEGIN;
		UPDATE lock_hot SET v = v + 1 WHERE id = 1;
		SELECT sum(sqrt(i::float8)) FROM (SELECT generate_series(1,60000000::bigint) AS i) s;
		COMMIT;" >/dev/null 2>&1 & )
	for i in $(seq 100); do
		# shellcheck disable=SC2086
		n=$(psql $_P -U "$ADMIN" -Atc "select count(*) from pg_locks l join pg_stat_activity a using (pid)
			where a.usename = 'report_user' and l.locktype = 'transactionid' and l.granted")
		[ "${n:-0}" -gt 0 ] && break
		sleep 0.1
	done
	[ "${n:-0}" -gt 0 ] || { echo "$1: the holder never took the lock; run not valid"; rm -f "$out"; return 1; }
	# shellcheck disable=SC2086
	hpid=$(psql $_P -U "$ADMIN" -Atc "select a.pid from pg_locks l join pg_stat_activity a using (pid)
		where a.usename = 'report_user' and l.locktype = 'transactionid' and l.granted limit 1")
	[ -n "$hpid" ] && [ -r "/proc/$hpid/schedstat" ] || { echo "$1: cannot find the holder; run not valid"; rm -f "$out"; return 1; }

	# Real-time priority if possible, so the load cannot starve the sampler.
	if command -v chrt >/dev/null 2>&1 && chrt -f 10 true 2>/dev/null; then
		chrt -f 10 bash -c "$_SAMPLER" _ "$hpid" "$out" &
	else
		bash -c "$_SAMPLER" _ "$hpid" "$out" &
	fi
	spid=$!
	for i in $(seq "$LOAD"); do
		# shellcheck disable=SC2086
		( timeout 25 psql $_P -U oltp_app -c "SELECT sum(sqrt(i::float8))
			FROM (SELECT generate_series(1,5000000000::bigint) AS i) s;" >/dev/null 2>&1 & )
	done
	sleep "$HEAD_START"
	t0=$EPOCHREALTIME
	# shellcheck disable=SC2086
	timeout 120 psql $_P -U oltp_app -c "UPDATE lock_hot SET v = v + 1 WHERE id = 1;" >/dev/null
	t1=$EPOCHREALTIME
	kill "$spid" 2>/dev/null; wait "$spid" 2>/dev/null

	_cpu_summary "$1" "$(cat /sys/kernel/sched_ext/state)" "$t0" "$t1" "$out"
	rm -f "$out"
	sleep 3
}

# _cpu_summary LABEL STATE T0 T1 FILE: print the line for one run.
_cpu_summary() {
	LC_ALL=C awk -v lbl="$1" -v st="$2" -v t0="$3" -v t1="$4" -v f="$5" 'BEGIN {
		have = 0
		while ((getline line < f) > 0) {
			split(line, a, " ")
			n++
			if (!have || a[1] <= t0) { c0 = a[2]; d0 = a[3]; have = 1 }   # last sample at or before t0
			if (a[1] >= t0) { m++ }
			ce = a[2]; de = a[3]                                       # last sample overall
		}
		if (!n) { printf "%s [%s] no samples\n", lbl, st; exit }
		w = t1 - t0
		cb = c0 / 1e9; cd = (ce - c0) / 1e9; rq = (de - d0) / 1e9
		printf "%s [%s] waiter waited %.2f s | holder cpu: before %.3f s, during %.3f s (%.0f%% of the wait), run-queue wait %.3f s, total %.3f s | samples %d (%d in the wait)\n",
		       lbl, st, w, cb, cd, (w > 0 ? 100 * cd / w : 0), rq, ce / 1e9, n, m
	}'
}

cpucompare() {   # cpucompare N: N alternating pairs
	local n="${1:?pairs}" i
	for i in $(seq 1 "$n"); do
		sched_on  >/dev/null || return 1; cpurun ours
		sched_off >/dev/null || return 1; cpurun eevdf
	done
	sched_off >/dev/null
	_lh_clean
}

[ "$(id -u)" -eq 0 ] || { echo "run as root, inside the test VM"; exit 1; }
_lh_setup || exit 1
case "${1:-}" in
quick)
	cpucompare 1
	;;
[1-9]*)
	cpucompare 1 > /dev/null 2>&1   # warm-up pair, discarded
	cpucompare "$1"
	;;
*)
	echo "usage: $0 quick | N"; exit 1 ;;
esac
