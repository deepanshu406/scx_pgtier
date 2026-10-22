#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Step 1 test for scx_pgtier: tier detection.
#
# Starts the scheduler (partial mode), puts one task in a "ufs-ts-" cgroup and
# one in a "ufs-bg-" cgroup, switches both to SCHED_EXT, and checks that the
# scheduler classified them as time-sensitive (class 1) and background (class 0).
#
# Run as root, ideally inside the VM:
#     ./test_tier_detect.sh [path/to/scx_pgtier]
#
set -u

BIN="${1:-./build/scx_pgtier}"
CG=/sys/fs/cgroup
TS_CG="$CG/ufs-ts-test"
BG_CG="$CG/ufs-bg-test"
SCHED_EXT=7

WORK="$(mktemp -d)"
SCHED_LOG="$WORK/sched.log"
TRACE_LOG="$WORK/trace.log"
SCHED_PID=""
TRACE_PID=""
TS_PID=""
BG_PID=""

fail() {
	echo "FAIL: $*" >&2
	echo "--- scheduler output (last 10 lines) ---" >&2
	tail -n 10 "$SCHED_LOG" >&2 2>/dev/null
	echo "--- trace output ---" >&2
	grep "ufs:" "$TRACE_LOG" >&2 2>/dev/null
	exit 1
}

cleanup() {
	[ -n "$TS_PID" ] && kill "$TS_PID" 2>/dev/null
	[ -n "$BG_PID" ] && kill "$BG_PID" 2>/dev/null
	[ -n "$SCHED_PID" ] && kill -INT "$SCHED_PID" 2>/dev/null
	[ -n "$TRACE_PID" ] && kill "$TRACE_PID" 2>/dev/null
	wait 2>/dev/null
	sleep 0.5
	rmdir "$TS_CG" "$BG_CG" 2>/dev/null
	rm -rf "$WORK"
}
trap cleanup EXIT

# ---- preconditions ----
[ "$(id -u)" -eq 0 ]            || { echo "FAIL: run as root" >&2; exit 1; }
[ -x "$BIN" ]                   || { echo "FAIL: $BIN not found; run make first" >&2; exit 1; }
[ -f "$CG/cgroup.controllers" ] || { echo "FAIL: cgroup v2 not mounted at $CG" >&2; exit 1; }
command -v python3 >/dev/null   || { echo "FAIL: python3 needed to switch tasks to SCHED_EXT" >&2; exit 1; }

TRACE=/sys/kernel/tracing
if [ ! -e "$TRACE/trace_pipe" ]; then
	mount -t tracefs nodev "$TRACE" 2>/dev/null
fi
if [ ! -e "$TRACE/trace_pipe" ]; then
	TRACE=/sys/kernel/debug/tracing
fi
[ -e "$TRACE/trace_pipe" ] || { echo "FAIL: trace_pipe not found (tracefs not mounted)" >&2; exit 1; }

# ---- start trace reader and scheduler ----
cat "$TRACE/trace_pipe" > "$TRACE_LOG" 2>&1 &
TRACE_PID=$!

"$BIN" > "$SCHED_LOG" 2>&1 &
SCHED_PID=$!

# wait until the scheduler prints its first stats line (= attached)
for _ in $(seq 1 50); do
	grep -q "^bg=" "$SCHED_LOG" 2>/dev/null && break
	kill -0 "$SCHED_PID" 2>/dev/null || fail "scheduler exited before attaching"
	sleep 0.1
done
grep -q "^bg=" "$SCHED_LOG" || fail "scheduler did not attach within 5 seconds"

# ---- create cgroups and tasks ----
mkdir "$TS_CG" "$BG_CG" || fail "could not create cgroups under $CG"

sleep 1000 &
TS_PID=$!
echo "$TS_PID" > "$TS_CG/cgroup.procs" || fail "could not move task into $TS_CG"

sleep 1000 &
BG_PID=$!
echo "$BG_PID" > "$BG_CG/cgroup.procs" || fail "could not move task into $BG_CG"

# ---- switch both to sched_ext; this triggers ufs_enable ----
for p in "$TS_PID" "$BG_PID"; do
	python3 -c 'import os,sys; os.sched_setscheduler(int(sys.argv[1]), int(sys.argv[2]), os.sched_param(0))' \
		"$p" "$SCHED_EXT" || fail "could not switch pid $p to SCHED_EXT"
done

sleep 2

# ---- check results ----
LAST="$(grep "^bg=" "$SCHED_LOG" | tail -n 1)"
BG_CNT="$(echo "$LAST" | sed -n 's/.*bg=\([0-9]*\).*/\1/p')"
TS_CNT="$(echo "$LAST" | sed -n 's/.*ts=\([0-9]*\).*/\1/p')"
UNK_CNT="$(echo "$LAST" | sed -n 's/.*unknown=\([0-9]*\).*/\1/p')"

echo "stats:  $LAST"

[ "${TS_CNT:-0}" -ge 1 ] || fail "expected ts >= 1, got '${TS_CNT:-}'"
[ "${BG_CNT:-0}" -ge 1 ] || fail "expected bg >= 1, got '${BG_CNT:-}'"

grep -q "pid=$TS_PID .*cgroup=ufs-ts-test class=1" "$TRACE_LOG" \
	|| fail "no trace line showing pid $TS_PID classified as time-sensitive"
grep -q "pid=$BG_PID .*cgroup=ufs-bg-test class=0" "$TRACE_LOG" \
	|| fail "no trace line showing pid $BG_PID classified as background"

echo "trace:"
grep "ufs:" "$TRACE_LOG"
echo "unknown count: ${UNK_CNT:-?} (should be 0 unless other tasks were switched to SCHED_EXT)"
echo "PASS: tier detection works"
