#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Priority-inversion test: how long does a time-sensitive transaction wait for
# a row lock that a background query holds, while time-sensitive load keeps
# every CPU busy?
#
#   1. a background-tier session (report_user) takes the row lock and then
#      needs about 1.3 s of CPU before it commits;
#   2. when the lock is held, LOAD time-sensitive sessions (oltp_app) start a
#      CPU-bound query each, enough to fill all CPUs;
#   3. HEAD_START seconds later a time-sensitive transaction updates the same
#      row. The time it waits is printed.
#
# Run as root inside the VM prepared by scripts/vm_setup.sh, from the repo root:
#
#     tests/test_lock_holder.sh quick     # one pair (scx_pgtier, then EEVDF)
#     tests/test_lock_holder.sh 5         # warm-up pair, then 5 alternating pairs
#
# Settings (environment): LOAD (default 16), HEAD_START (default 0.3 s), SCX_DB
# (connection string for the scheduler's -d option; default: the settings of
# scripts/bench_funcs.sh, which turns the lock-holder boost on), and the
# variables of scripts/bench_funcs.sh (ADMIN, SOCK, PORT, DB).
#
# The load sessions are started with "timeout" and their PostgreSQL backends
# keep running after the client is gone (PostgreSQL does not notice a lost
# client during a CPU-only query), so every run first terminates the leftovers
# of the previous one.

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

# Terminate the sessions left by the previous run.
_lh_clean() {
	# shellcheck disable=SC2086
	psql $_P -U "$ADMIN" -Atc "select count(pg_terminate_backend(pid)) from pg_stat_activity
		where usename in ('oltp_app','report_user') and pid <> pg_backend_pid()" >/dev/null
	sleep 2
}

# lockrun LABEL: one run. Prints one line.
lockrun() {
	local n=0 i t0 t1
	_lh_clean
	# shellcheck disable=SC2086
	( timeout 90 psql $_P -U report_user -c "BEGIN;
		UPDATE lock_hot SET v = v + 1 WHERE id = 1;
		SELECT sum(sqrt(i::float8)) FROM (SELECT generate_series(1,60000000::bigint) AS i) s;
		COMMIT;" >/dev/null 2>&1 & )
	# Wait (at most 10 s) until the holder really owns the lock, before the load starts.
	for i in $(seq 100); do
		# shellcheck disable=SC2086
		n=$(psql $_P -U "$ADMIN" -Atc "select count(*) from pg_locks l join pg_stat_activity a using (pid)
			where a.usename = 'report_user' and l.locktype = 'transactionid' and l.granted")
		[ "${n:-0}" -gt 0 ] && break
		sleep 0.1
	done
	[ "${n:-0}" -gt 0 ] || { echo "$1: the holder never took the lock; run not valid"; return 1; }
	for i in $(seq "$LOAD"); do
		# shellcheck disable=SC2086
		( timeout 25 psql $_P -U oltp_app -c "SELECT sum(sqrt(i::float8))
			FROM (SELECT generate_series(1,5000000000::bigint) AS i) s;" >/dev/null 2>&1 & )
	done
	sleep "$HEAD_START"
	t0=$(date +%s.%N)
	# shellcheck disable=SC2086
	timeout 120 psql $_P -U oltp_app -c "UPDATE lock_hot SET v = v + 1 WHERE id = 1;" >/dev/null
	t1=$(date +%s.%N)
	echo "$1 [$(cat /sys/kernel/sched_ext/state)] wait for the lock: $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f", b - a }') s"
	sleep 3
}

lockcompare() {   # lockcompare N: N alternating pairs
	local n="${1:?pairs}" i
	for i in $(seq 1 "$n"); do
		sched_on  >/dev/null || return 1; lockrun ours
		sched_off >/dev/null || return 1; lockrun eevdf
	done
	sched_off >/dev/null
	_lh_clean
}

[ "$(id -u)" -eq 0 ] || { echo "run as root, inside the test VM"; exit 1; }
_lh_setup || exit 1
case "${1:-}" in
quick)
	lockcompare 1
	;;
[1-9]*)
	lockcompare 1 > /dev/null 2>&1   # warm-up pair, discarded
	lockcompare "$1"
	;;
*)
	echo "usage: $0 quick | N"; exit 1 ;;
esac
