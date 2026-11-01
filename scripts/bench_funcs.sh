# SPDX-License-Identifier: GPL-2.0
#
# Benchmark helpers for scx_pgtier. Source this file, do not run it:
#
#     source scripts/bench_funcs.sh
#     compare 5 mixed ""            # 5 alternating pairs, scheduler on / off
#     compare 5 solo  ""            # same, without the report queries
#     compare 5 mixed "-R 12000"    # fixed transaction rate
#
# Run as root, from the repository root, inside the VM prepared by scripts/vm_setup.sh. See docs/BENCHMARK.md.
#
# Settings (override before sourcing, for example:  PORT=5432 source scripts/bench_funcs.sh):
#   ADMIN   OS/database superuser that owns the cluster  (default: owner of the repository directory)
#   SOCK    PostgreSQL socket directory                   (default: /tmp)
#   PORT    PostgreSQL port                               (default: 5433)
#   DB      benchmark database                            (default: bench)
#   HEAVY   number of report-query loops in a mixed run   (default: 8)
#   SECS    pgbench duration in seconds                   (default: 30)

_BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # repository root
ADMIN="${ADMIN:-$(stat -c %U "$_BENCH_DIR")}"
SOCK="${SOCK:-/tmp}"
PORT="${PORT:-5433}"
DB="${DB:-bench}"
HEAVY="${HEAVY:-8}"
SECS="${SECS:-30}"

# Use the newest PostgreSQL client tools on this machine.
_pgbin="$(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -V | tail -n 1)"
[ -n "$_pgbin" ] && export PATH="$_pgbin:$PATH"

# ---- scheduler on / off -----------------------------------------------------

# sched_off: stop scx_pgtier; the kernel goes back to EEVDF.
sched_off() {
  pkill -x scx_pgtier.run 2>/dev/null
  pkill -x scx_pgtier 2>/dev/null
  local i
  for i in $(seq 1 50); do
    [ "$(cat /sys/kernel/sched_ext/state)" = disabled ] && return 0
    sleep 0.2
  done
  echo "scheduler did not stop" >&2
  return 1
}

# sched_on [BINARY]: start scx_pgtier from a private copy of the binary.
# The copy (after dropping the page cache) avoids crashes from a stale cached
# binary when it was just rebuilt on the host.
sched_on() {
  local bin="${1:-$_BENCH_DIR/build/scx_pgtier}"
  [ -x "$bin" ] || { echo "no binary at $bin; build first (docs/BUILD.md)" >&2; return 1; }
  sched_off || return 1
  sync; echo 3 > /proc/sys/vm/drop_caches
  cp "$bin" /tmp/scx_pgtier.run
  local args=(); [ -n "${SCX_DB:-}" ] && args=(-d "$SCX_DB")
  ( setsid /tmp/scx_pgtier.run "${args[@]}" > /tmp/sched.log 2>&1 < /dev/null & )   # subshell: no job-control noise
  sleep 3
  [ "$(cat /sys/kernel/sched_ext/state)" = enabled ] || { echo "scheduler did not start:" >&2; tail -n 5 /tmp/sched.log >&2; return 1; }
}

# ---- measurement -------------------------------------------------------------

# pct: latency percentiles of the last pgbench run (log column 3, microseconds).
pct() {
  cat /tmp/pgb.* | awk '{print $3}' | sort -n > /tmp/lat.txt
  local n; n=$(wc -l < /tmp/lat.txt)
  echo "txns=$n"
  echo "p50=$(sed -n "$((n*50/100))p"    /tmp/lat.txt)us"
  echo "p99=$(sed -n "$((n*99/100))p"    /tmp/lat.txt)us"
  echo "p99.9=$(sed -n "$((n*999/1000))p" /tmp/lat.txt)us"
  echo "max=$(tail -n 1 /tmp/lat.txt)us"
}

# Every run starts from the same state: small tables, empty history.
# Without this, pgbench_branches and pgbench_tellers bloat (autovacuum is off)
# and later runs get slower than earlier ones.
_reset_tables() {
  psql -X -q -U "$ADMIN" -h "$SOCK" -p "$PORT" "$DB" \
    -c "TRUNCATE pgbench_history" \
    -c "VACUUM FULL pgbench_branches, pgbench_tellers"
}

_pgbench() {   # _pgbench "EXTRA PGBENCH OPTIONS"
  pgbench -n $1 -l --log-prefix=/tmp/pgb -h "$SOCK" -p "$PORT" -U oltp_app \
    -c 8 -j 4 -T "$SECS" "$DB" | grep "^tps" | awk '{print $3}'
}

# one LABEL "PGBENCH OPTIONS": pgbench plus HEAVY report-query loops.
# Prints one line: label, scheduler state, tps, latency percentiles, report queries done.
one() {
  _reset_tables
  rm -f /tmp/pgb.* /tmp/heavy_done
  ( # subshell: no job-control noise in an interactive shell
    local i
    for i in $(seq 1 "$HEAVY"); do
      timeout $((SECS + 15)) bash -c '
        while true; do
          psql -q -U report_user -h "$0" -p "$1" "$2" \
            -c "SELECT sum(sqrt(i::float8)) FROM (SELECT generate_series(1,60000000) AS i) s;" \
            >/dev/null 2>&1 && echo x >> /tmp/heavy_done
        done' "$SOCK" "$PORT" "$DB" 2>/dev/null &
    done
  )
  sleep 5
  local T P
  T=$(_pgbench "$2")
  P=$(pct | tr '\n' ' ')
  sleep 17   # let the report loops run out; this keeps the count comparable
  echo "$1 [$(cat /sys/kernel/sched_ext/state)] tps=$T $P heavy=$(wc -l < /tmp/heavy_done)"
}

# solo LABEL "PGBENCH OPTIONS": pgbench alone, no report queries.
solo() {
  _reset_tables
  rm -f /tmp/pgb.*
  local T P
  T=$(_pgbench "$2")
  P=$(pct | tr '\n' ' ')
  echo "$1 [$(cat /sys/kernel/sched_ext/state)] tps=$T $P"
}

# compare N MODE "PGBENCH OPTIONS": N alternating pairs, scheduler on, then off.
#   MODE is "mixed" (pgbench + report queries) or "solo" (pgbench only).
# Take the median of each column over the N lines per label.
compare() {
  local n="${1:?pairs}" mode="${2:?mixed|solo}" opts="${3:-}" i
  case "$mode" in mixed|solo) ;; *) echo "mode must be mixed or solo" >&2; return 1;; esac
  local run=one; [ "$mode" = solo ] && run=solo
  for i in $(seq 1 "$n"); do
    sched_on  >/dev/null || return 1; $run ours  "$opts"
    sched_off >/dev/null || return 1; $run eevdf "$opts"
  done
  sched_off >/dev/null
}

# summarize FILE: median of every column per label, from saved compare output.
#   compare 5 mixed "" 2>&1 | tee /tmp/step1.txt ; summarize /tmp/step1.txt
summarize() {
  awk '
  function med(lab, key,   i, j, t, m, b) {
    m = cnt[lab, key]
    for (i = 1; i <= m; i++) b[i] = d[lab, key, i]
    for (i = 1; i <= m; i++) for (j = i + 1; j <= m; j++) if (b[j] < b[i]) { t = b[i]; b[i] = b[j]; b[j] = t }
    return (m % 2) ? b[(m + 1) / 2] : (b[m / 2] + b[m / 2 + 1]) / 2
  }
  $1 ~ /(ours|eevdf)$/ {
    lab = ($1 ~ /ours$/) ? "ours" : "eevdf"
    runs[lab]++
    for (i = 2; i <= NF; i++) {
      p = index($i, "="); if (!p) continue
      key = substr($i, 1, p - 1); v = substr($i, p + 1); sub(/us$/, "", v)
      if (key ~ /^(tps|p50|p99|p99.9|max|heavy)$/) { cnt[lab, key]++; d[lab, key, cnt[lab, key]] = v + 0 }
    }
  }
  END {
    printf "%d runs of ours, %d of eevdf (medians)\n", runs["ours"], runs["eevdf"]
    printf "%-8s %12s %12s %8s\n", "", "ours", "eevdf", "change"
    n = split("tps p50 p99 p99.9 max heavy", K, " ")
    for (k = 1; k <= n; k++) {
      key = K[k]; if (!cnt["ours", key] || !cnt["eevdf", key]) continue
      a = med("ours", key); b = med("eevdf", key)
      printf "%-8s %12.0f %12.0f %+7.0f%%\n", key, a, b, (a - b) / b * 100
    }
  }' "${1:?file}"
}
