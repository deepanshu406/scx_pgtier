#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Step 2 test for scx_pgtier: the express lane.
#
# Fills every CPU with busy background tasks (cgroup ufs-bg-hog), then measures
# how late a periodic sleeper wakes up, twice with the same scheduler:
#   - once in a background cgroup  (ufs-bg-lat)
#   - once in a time-sensitive one (ufs-ts-lat)
# The time-sensitive sleeper should wake much closer to its deadline, and the
# scheduler's preempt counter should be above zero.
#
# Run as root, ideally inside the VM (needs gcc and python3):
#     ./test_express_lane.sh [path/to/scx_pgtier] [N] [PERIOD_US]
#
set -u

BIN="${1:-./build/scx_pgtier}"
N="${2:-2000}"
PERIOD_US="${3:-1000}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CG=/sys/fs/cgroup
HOG_CG="$CG/ufs-bg-hog"
BGLAT_CG="$CG/ufs-bg-lat"
TSLAT_CG="$CG/ufs-ts-lat"
SCHED_EXT=7

WORK="$(mktemp -d)"
SCHED_LOG="$WORK/sched.log"
SCHED_PID=""
HOG_PIDS=""

fail() {
	echo "FAIL: $*" >&2
	echo "--- scheduler output (last 10 lines) ---" >&2
	tail -n 10 "$SCHED_LOG" >&2 2>/dev/null
	exit 1
}

cleanup() {
	for p in $HOG_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$SCHED_PID" ] && kill -INT "$SCHED_PID" 2>/dev/null
	wait 2>/dev/null
	sleep 0.5
	rmdir "$HOG_CG" "$BGLAT_CG" "$TSLAT_CG" 2>/dev/null
	rm -rf "$WORK"
}
trap cleanup EXIT

to_ext() {
	python3 -c 'import os,sys; os.sched_setscheduler(int(sys.argv[1]), int(sys.argv[2]), os.sched_param(0))' \
		"$1" "$SCHED_EXT"
}

# ---- preconditions ----
[ "$(id -u)" -eq 0 ]            || { echo "FAIL: run as root" >&2; exit 1; }
[ -x "$BIN" ]                   || { echo "FAIL: $BIN not found; run make first" >&2; exit 1; }
[ -f "$CG/cgroup.controllers" ] || { echo "FAIL: cgroup v2 not mounted at $CG" >&2; exit 1; }
command -v python3 >/dev/null   || { echo "FAIL: python3 needed" >&2; exit 1; }
command -v gcc >/dev/null       || { echo "FAIL: gcc needed to build wakelat" >&2; exit 1; }

gcc -O2 -Wall -o "$WORK/wakelat" "$HERE/wakelat.c" || { echo "FAIL: could not build wakelat" >&2; exit 1; }

NCPU="$(nproc)"

# ---- start the scheduler ----
"$BIN" > "$SCHED_LOG" 2>&1 &
SCHED_PID=$!
for _ in $(seq 1 50); do
	grep -q "^bg=" "$SCHED_LOG" 2>/dev/null && break
	kill -0 "$SCHED_PID" 2>/dev/null || fail "scheduler exited before attaching"
	sleep 0.1
done
grep -q "^bg=" "$SCHED_LOG" || fail "scheduler did not attach within 5 seconds"

mkdir "$HOG_CG" "$BGLAT_CG" "$TSLAT_CG" || fail "could not create cgroups under $CG"

# ---- one busy loop per CPU, in the background tier, under sched_ext ----
echo "starting $NCPU background hogs..."
for _ in $(seq 1 "$NCPU"); do
	bash -c "echo \$\$ > $HOG_CG/cgroup.procs; while :; do :; done" &
	pid=$!
	HOG_PIDS="$HOG_PIDS $pid"
done
sleep 0.5
for pid in $HOG_PIDS; do
	to_ext "$pid" || fail "could not switch hog $pid to SCHED_EXT"
done
sleep 1

# ---- measure: background sleeper, then time-sensitive sleeper ----
run_lat() {   # $1 = cgroup dir
	bash -c "echo \$\$ > $1/cgroup.procs; exec $WORK/wakelat $N $PERIOD_US"
}

echo "measuring background sleeper..."
BG_RES="$(run_lat "$BGLAT_CG" 2>&1)"
echo "measuring time-sensitive sleeper..."
TS_RES="$(run_lat "$TSLAT_CG" 2>&1)"

echo
echo "background:      $BG_RES"
echo "time-sensitive:  $TS_RES"

BG_P99="$(echo "$BG_RES" | sed -n 's/.*p99_us=\([0-9]*\).*/\1/p')"
TS_P99="$(echo "$TS_RES" | sed -n 's/.*p99_us=\([0-9]*\).*/\1/p')"
[ -n "$BG_P99" ] && [ -n "$TS_P99" ] || fail "could not read latency results (is SCHED_EXT available?)"

sleep 1
LAST="$(grep "^bg=" "$SCHED_LOG" | tail -n 1)"
PREEMPT="$(echo "$LAST" | sed -n 's/.*ts_preempt=\([0-9]*\).*/\1/p')"
echo "stats:           $LAST"
echo

[ "${PREEMPT:-0}" -ge 1 ] || fail "expected ts_preempt >= 1, got '${PREEMPT:-}'"
[ "$TS_P99" -lt "$BG_P99" ] || fail "time-sensitive p99 (${TS_P99}us) is not lower than background p99 (${BG_P99}us)"

echo "PASS: time-sensitive p99 ${TS_P99}us vs background p99 ${BG_P99}us, $PREEMPT preempt kicks"
