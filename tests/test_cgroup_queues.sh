#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Step 3A test for scx_pgtier: per-cgroup records and queues.
#
# Starts the scheduler, then creates cgroups and checks the counters that the
# scheduler prints:
#   2 x ufs-bg-*   -> cg_back  +2   (a record and a queue each)
#   1 x ufs-ts-*   -> cg_time  +1   (a record, no queue)
#   1 x other      -> cg_other +1   (a record, no queue)
# Then removes them and checks:
#   cg_destroyed +2                 (the two queues were destroyed)
# cg_err must not change.
#
# Run as root, ideally inside the VM:
#     ./test_cgroup_queues.sh [path/to/scx_pgtier]
#
set -u

BIN="${1:-./build/scx_pgtier}"
CG=/sys/fs/cgroup
BG1="$CG/ufs-bg-a"
BG2="$CG/ufs-bg-b"
TS1="$CG/ufs-ts-a"
OTH="$CG/other-test"

WORK="$(mktemp -d)"
SCHED_LOG="$WORK/sched.log"
TRACE_LOG="$WORK/trace.log"
SCHED_PID=""
TRACE_PID=""

fail() {
	echo "FAIL: $*" >&2
	echo "--- scheduler output (last 5 lines) ---" >&2
	tail -n 5 "$SCHED_LOG" >&2 2>/dev/null
	echo "--- trace output (cgroup lines) ---" >&2
	grep "cgroup_" "$TRACE_LOG" >&2 2>/dev/null
	exit 1
}

cleanup() {
	[ -n "$SCHED_PID" ] && kill -INT "$SCHED_PID" 2>/dev/null
	[ -n "$TRACE_PID" ] && kill "$TRACE_PID" 2>/dev/null
	wait 2>/dev/null
	sleep 0.5
	rmdir "$BG1" "$BG2" "$TS1" "$OTH" 2>/dev/null
	rm -rf "$WORK"
}
trap cleanup EXIT

# value of one counter in the last stats line, e.g. stat cg_back
stat() {
	grep "^bg=" "$SCHED_LOG" | tail -n 1 | sed -n "s/.* $1=\([0-9]*\).*/\1/p"
}

# ---- preconditions ----
[ "$(id -u)" -eq 0 ]            || { echo "FAIL: run as root" >&2; exit 1; }
[ -x "$BIN" ]                   || { echo "FAIL: $BIN not found; run make first" >&2; exit 1; }
[ -f "$CG/cgroup.controllers" ] || { echo "FAIL: cgroup v2 not mounted at $CG" >&2; exit 1; }

TRACE=/sys/kernel/tracing
if [ ! -e "$TRACE/trace_pipe" ]; then
	mount -t tracefs nodev "$TRACE" 2>/dev/null
fi
if [ ! -e "$TRACE/trace_pipe" ]; then
	TRACE=/sys/kernel/debug/tracing
fi
[ -e "$TRACE/trace_pipe" ] || { echo "FAIL: trace_pipe not found (tracefs not mounted)" >&2; exit 1; }

# The cpu controller must be on for new cgroups to get their own cgroup_init.
if ! grep -qw cpu "$CG/cgroup.subtree_control"; then
	echo +cpu > "$CG/cgroup.subtree_control" 2>/dev/null \
		|| fail "could not enable the cpu controller in $CG/cgroup.subtree_control"
	echo "enabled the cpu controller in $CG/cgroup.subtree_control"
fi

# ---- start trace reader and scheduler ----
cat "$TRACE/trace_pipe" > "$TRACE_LOG" 2>&1 &
TRACE_PID=$!

"$BIN" > "$SCHED_LOG" 2>&1 &
SCHED_PID=$!

for _ in $(seq 1 50); do
	grep -q "^bg=" "$SCHED_LOG" 2>/dev/null && break
	kill -0 "$SCHED_PID" 2>/dev/null || fail "scheduler exited before attaching"
	sleep 0.1
done
grep -q "^bg=" "$SCHED_LOG" || fail "scheduler did not attach within 5 seconds"
sleep 1.5

B_BACK="$(stat cg_back)"; B_TIME="$(stat cg_time)"; B_OTHER="$(stat cg_other)"
B_DESTROYED="$(stat cg_destroyed)"; B_ERR="$(stat cg_err)"
echo "baseline: cg_back=$B_BACK cg_time=$B_TIME cg_other=$B_OTHER cg_destroyed=$B_DESTROYED cg_err=$B_ERR"

# ---- create cgroups ----
mkdir "$BG1" "$BG2" "$TS1" "$OTH" || fail "could not create cgroups under $CG"
sleep 2

A_BACK="$(stat cg_back)"; A_TIME="$(stat cg_time)"; A_OTHER="$(stat cg_other)"; A_ERR="$(stat cg_err)"
echo "after create: cg_back=$A_BACK cg_time=$A_TIME cg_other=$A_OTHER cg_err=$A_ERR"

[ $((A_BACK - B_BACK)) -eq 2 ]   || fail "expected cg_back +2, got +$((A_BACK - B_BACK)) (is the cpu controller enabled?)"
[ $((A_TIME - B_TIME)) -eq 1 ]   || fail "expected cg_time +1, got +$((A_TIME - B_TIME))"
[ $((A_OTHER - B_OTHER)) -eq 1 ] || fail "expected cg_other +1, got +$((A_OTHER - B_OTHER))"
[ "$A_ERR" -eq "$B_ERR" ]        || fail "cg_err changed: $B_ERR -> $A_ERR"

# ---- remove cgroups ----
rmdir "$BG1" "$BG2" "$TS1" "$OTH" || fail "could not remove cgroups"
sleep 2

D_DESTROYED="$(stat cg_destroyed)"; D_ERR="$(stat cg_err)"
echo "after remove: cg_destroyed=$D_DESTROYED cg_err=$D_ERR"

[ $((D_DESTROYED - B_DESTROYED)) -eq 2 ] \
	|| fail "expected cg_destroyed +2, got +$((D_DESTROYED - B_DESTROYED))"
[ "$D_ERR" -eq "$B_ERR" ] || fail "cg_err changed: $B_ERR -> $D_ERR"

echo "trace:"
grep "cgroup_" "$TRACE_LOG"
echo "PASS: cgroup records and queues are created and destroyed"
