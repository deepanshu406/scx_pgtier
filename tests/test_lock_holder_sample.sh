#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Where does the lock holder spend its time while a time-sensitive transaction
# waits for its row lock?
#
# Same setup as tests/test_lock_holder.sh (a background session holds a row
# lock and needs about 1.3 s of CPU; LOAD time-sensitive sessions fill all
# CPUs; a time-sensitive transaction then updates the locked row), plus two
# measurements of the HOLDER over the window in which the waiter waits:
#
#   cpu   = CPU time the holder got (from /proc/PID/schedstat), and that time
#           as a share of the window. Low share = the holder waited for a CPU.
#   waits = what pg_stat_activity says the holder was doing, sampled every
#           SAMPLE seconds. "running" = no wait event. LWLock:..., IO:... etc.
#           = it was waiting inside PostgreSQL, not for a CPU.
#
# Run as root inside the test VM, from the repo root:
#
#     tests/test_lock_holder_sample.sh quick   # one pair (ours, then EEVDF)
#     tests/test_lock_holder_sample.sh 5       # warm-up pair, then 5 pairs
#
# Settings (environment): LOAD (16), HEAD_START (0.3 s), SAMPLE (0.1 s), SCX_DB.
# The sampler starts a short psql every SAMPLE seconds; that is a small extra
# load, the same for both schedulers.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../scripts/bench_funcs.sh
source "$DIR/scripts/bench_funcs.sh"

LOAD="${LOAD:-16}"
HEAD_START="${HEAD_START:-0.3}"
SAMPLE="${SAMPLE:-0.1}"
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

# _sampler PID FILE: one line "time what" every SAMPLE seconds, until killed.
_sampler() {
	local pid="$1" out="$2" w
	while :; do
		# shellcheck disable=SC2086
		w=$(psql $_P -U "$ADMIN" -Atc "select coalesce(wait_event_type || ':' || wait_event, 'running')
			from pg_stat_activity where pid = $pid")
		# CPU time so far (ns). The holder exits right after it commits, so the
		# value must be recorded here, while it is alive, not after the wait.
		echo "$(date +%s.%N) ${w:-gone} $(cut -d' ' -f1 "/proc/$pid/schedstat" 2>/dev/null || echo 0)" >> "$out"
		sleep "$SAMPLE"
	done
}

# samprun LABEL: one run. Prints one line.
samprun() {
	local n=0 i t0 t1 hpid c0 c1 spid out
	out=$(mktemp /tmp/lh_samples.XXXXXX)
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

	_sampler "$hpid" "$out" &
	spid=$!
	for i in $(seq "$LOAD"); do
		# shellcheck disable=SC2086
		( timeout 25 psql $_P -U oltp_app -c "SELECT sum(sqrt(i::float8))
			FROM (SELECT generate_series(1,5000000000::bigint) AS i) s;" >/dev/null 2>&1 & )
	done
	sleep "$HEAD_START"
	c0=$(cut -d' ' -f1 "/proc/$hpid/schedstat")
	t0=$(date +%s.%N)
	# shellcheck disable=SC2086
	timeout 120 psql $_P -U oltp_app -c "UPDATE lock_hot SET v = v + 1 WHERE id = 1;" >/dev/null
	t1=$(date +%s.%N)
	c1=$(cut -d' ' -f1 "/proc/$hpid/schedstat" 2>/dev/null || echo "$c0")
	kill "$spid" 2>/dev/null; wait "$spid" 2>/dev/null

	awk -v lbl="$1" -v st="$(cat /sys/kernel/sched_ext/state)" \
	    -v t0="$t0" -v t1="$t1" -v c0="$c0" -v c1="$c1" -v f="$out" 'BEGIN {
		w = t1 - t0; last = c1
		while ((getline line < f) > 0) {
			split(line, a, " ")
			if (a[1] >= t0 && a[1] <= t1) { cnt[a[2]]++; tot++ }
			if (a[1] >= t0 && a[3] > last) last = a[3]
		}
		cpu = (last - c0) / 1e9
		s = ""
		for (k in cnt) s = s sprintf(" %s=%d", k, cnt[k])
		printf "%s [%s] wait %.2f s, holder cpu %.2f s (%.0f%% of the wait), samples %d:%s\n",
		       lbl, st, w, cpu, (w > 0 ? 100 * cpu / w : 0), tot, s
	}'
	rm -f "$out"
	sleep 3
}

sampcompare() {   # sampcompare N: N alternating pairs
	local n="${1:?pairs}" i
	for i in $(seq 1 "$n"); do
		sched_on  >/dev/null || return 1; samprun ours
		sched_off >/dev/null || return 1; samprun eevdf
	done
	sched_off >/dev/null
	_lh_clean
}

[ "$(id -u)" -eq 0 ] || { echo "run as root, inside the test VM"; exit 1; }
_lh_setup || exit 1
case "${1:-}" in
quick)
	sampcompare 1
	;;
[1-9]*)
	sampcompare 1 > /dev/null 2>&1   # warm-up pair, discarded
	sampcompare "$1"
	;;
*)
	echo "usage: $0 quick | N"; exit 1 ;;
esac
