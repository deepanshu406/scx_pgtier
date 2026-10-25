#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Step 3C test for scx_pgtier: weighted cgroup virtual clocks.
#
# Creates two background cgroups with cpu.weight 200 and 100, puts one busy
# loop per CPU in each (so the cgroups compete for every CPU), lets them run,
# then measures how much CPU each cgroup got in a fixed window (the difference
# of usage_usec in cpu.stat). With weights 200:100 the split should be about
# 2:1. It also checks that dispatch ran (disp_ok grew) and that no errors were
# counted (cg_err unchanged).
#
# Run as root, ideally inside the VM (needs python3):
#     ./test_weighted_cgroups.sh [path/to/scx_pgtier] [WINDOW_SECONDS]
#
set -u

BIN="${1:-./build/scx_pgtier}"
WINDOW="${2:-20}"
WEIGHT_A=200
WEIGHT_B=100
TOLERANCE_PCT=10          # allowed deviation from the expected ratio
SCHED_EXT=7

CG=/sys/fs/cgroup
CG_A="$CG/ufs-bg-wa"
CG_B="$CG/ufs-bg-wb"

WORK="$(mktemp -d)"
SCHED_LOG="$WORK/sched.log"
SCHED_PID=""
HOG_PIDS=""

fail() {
	echo "FAIL: $*" >&2
	echo "--- scheduler output (last 8 lines) ---" >&2
	tail -n 8 "$SCHED_LOG" >&2 2>/dev/null
	exit 1
}

cleanup() {
	for p in $HOG_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$SCHED_PID" ] && kill -INT "$SCHED_PID" 2>/dev/null
	wait 2>/dev/null
	sleep 0.5
	rmdir "$CG_A" "$CG_B" 2>/dev/null
	rm -rf "$WORK"
}
trap cleanup EXIT

# value of one counter in the last stats line, e.g. stat disp_ok
stat() {
	grep "^bg=" "$SCHED_LOG" | tail -n 1 | sed -n "s/.* $1=\([0-9]*\).*/\1/p"
}

# CPU time (usec) used so far by a cgroup
usage() {
	awk '/^usage_usec/ { print $2 }' "$1/cpu.stat"
}

# ---- preconditions ----
[ "$(id -u)" -eq 0 ]            || { echo "FAIL: run as root" >&2; exit 1; }
[ -x "$BIN" ]                   || { echo "FAIL: $BIN not found; run make first" >&2; exit 1; }
[ -f "$CG/cgroup.controllers" ] || { echo "FAIL: cgroup v2 not mounted at $CG" >&2; exit 1; }
command -v python3 >/dev/null   || { echo "FAIL: python3 needed" >&2; exit 1; }

# The cpu controller must be on for the new cgroups to get cgroup_init and cpu.weight.
if ! grep -qw cpu "$CG/cgroup.subtree_control"; then
	echo +cpu > "$CG/cgroup.subtree_control" 2>/dev/null \
		|| fail "could not enable the cpu controller in $CG/cgroup.subtree_control"
	echo "enabled the cpu controller in $CG/cgroup.subtree_control"
fi

NCPU="$(nproc)"

# The hog: switch itself to SCHED_EXT, then burn CPU forever.
cat > "$WORK/hog.py" <<EOF
import os
os.sched_setscheduler(0, $SCHED_EXT, os.sched_param(0))
while True:
    pass
EOF

# ---- start the scheduler ----
"$BIN" > "$SCHED_LOG" 2>&1 &
SCHED_PID=$!
for _ in $(seq 1 50); do
	grep -q "^bg=" "$SCHED_LOG" 2>/dev/null && break
	kill -0 "$SCHED_PID" 2>/dev/null || fail "scheduler exited before attaching"
	sleep 0.1
done
grep -q "^bg=" "$SCHED_LOG" || fail "scheduler did not attach within 5 seconds"
sleep 1.5

B_ERR="$(stat cg_err)"
B_DISP="$(stat disp_ok)"

# ---- two cgroups with different weights ----
mkdir "$CG_A" "$CG_B" || fail "could not create cgroups under $CG"
echo "$WEIGHT_A" > "$CG_A/cpu.weight" || fail "could not set cpu.weight on $CG_A"
echo "$WEIGHT_B" > "$CG_B/cpu.weight" || fail "could not set cpu.weight on $CG_B"
sleep 1

# ---- one hog per CPU in each cgroup ----
echo "starting $NCPU hogs in each of two cgroups (weights $WEIGHT_A and $WEIGHT_B)..."
for dir in "$CG_A" "$CG_B"; do
	for _ in $(seq 1 "$NCPU"); do
		bash -c "echo \$\$ > $dir/cgroup.procs; exec python3 $WORK/hog.py" &
		HOG_PIDS="$HOG_PIDS $!"
	done
done

# let every hog start and the clocks settle before measuring
sleep 4
kill -0 "$SCHED_PID" 2>/dev/null || fail "scheduler exited while the hogs started"

# ---- measure over a fixed window ----
A1="$(usage "$CG_A")"; B1="$(usage "$CG_B")"
echo "measuring for ${WINDOW}s..."
sleep "$WINDOW"
A2="$(usage "$CG_A")"; B2="$(usage "$CG_B")"

DA=$((A2 - A1))
DB=$((B2 - B1))
[ "$DB" -gt 0 ] || fail "cgroup B used no CPU in the window (did the hogs start under SCHED_EXT?)"

RATIO_X100=$((DA * 100 / DB))
EXPECT_X100=$((WEIGHT_A * 100 / WEIGHT_B))
LOW=$((EXPECT_X100 * (100 - TOLERANCE_PCT) / 100))
HIGH=$((EXPECT_X100 * (100 + TOLERANCE_PCT) / 100))

A_E="$(stat disp_ok)"; E_ERR="$(stat cg_err)"
LAST="$(grep "^bg=" "$SCHED_LOG" | tail -n 1)"

echo
echo "cgroup A (weight $WEIGHT_A): ${DA} usec"
echo "cgroup B (weight $WEIGHT_B): ${DB} usec"
echo "ratio A/B: $((RATIO_X100 / 100)).$(printf '%02d' $((RATIO_X100 % 100)))  (expected $((EXPECT_X100 / 100)).$(printf '%02d' $((EXPECT_X100 % 100))) +/- ${TOLERANCE_PCT}%)"
echo "stats: $LAST"
echo "slots:"
tail -n 12 "$SCHED_LOG" | grep "^  slot"
echo

[ "${A_E:-0}" -gt "${B_DISP:-0}" ] || fail "disp_ok did not grow (${B_DISP:-} -> ${A_E:-})"
[ "${E_ERR:-0}" -eq "${B_ERR:-0}" ] || fail "cg_err changed: ${B_ERR:-} -> ${E_ERR:-}"
[ "$RATIO_X100" -ge "$LOW" ] && [ "$RATIO_X100" -le "$HIGH" ] \
	|| fail "ratio $RATIO_X100/100 is outside the allowed range $LOW/100 to $HIGH/100"

echo "PASS: weights $WEIGHT_A:$WEIGHT_B gave a CPU split of $((RATIO_X100 / 100)).$(printf '%02d' $((RATIO_X100 % 100))) : 1"
