#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Bring a test VM from a fresh boot to "ready to benchmark":
#   cgroups -> PostgreSQL (inside ufs-pg/main) -> roles, database, data -> scheduler
#
# Run as root INSIDE the VM (see docs/BUILD.md), from anywhere:
#     sudo scripts/vm_setup.sh            # set everything up and start the scheduler
#     sudo scripts/vm_setup.sh --no-sched # same, but leave the scheduler off (EEVDF baseline)
#
# It restarts PostgreSQL if one is already running on the same data directory.
# For that reason it refuses to run on a machine that does not look like a virtme-ng VM;
# pass --force if you really want to run it elsewhere.
#
# Safe to run again: every step checks before it changes anything.
#
# Settings (environment variables, all optional):
#   PGUSER_OS  OS user that owns the cluster      (default: the user who called sudo,
#                                                  else the owner of the repository directory)
#   PGBIN      PostgreSQL bin directory           (default: newest /usr/lib/postgresql/*/bin)
#   PGDATA     data directory                     (default: /home/$PGUSER_OS/pgdata)
#   PORT       server port                        (default: 5433)
#   SOCK       socket directory                   (default: /tmp)
#   DB         benchmark database                 (default: bench)
#   SCALE      pgbench scale factor               (default: 10)
set -eu

DIR="$(cd "$(dirname "$0")/.." && pwd)"
START_SCHED=1
FORCE=0
for a in "$@"; do
	case "$a" in
	--no-sched) START_SCHED=0 ;;
	--force)    FORCE=1 ;;
	*) echo "unknown option: $a"; exit 1 ;;
	esac
done

[ "$(id -u)" -eq 0 ] || { echo "run as root"; exit 1; }

if [ $FORCE -eq 0 ] && ! grep -q virtme /proc/mounts; then
	echo "This does not look like a virtme-ng VM. This script restarts PostgreSQL and"
	echo "changes cgroups; run it inside the VM, or pass --force to run it here."
	exit 1
fi

if [ -z "${PGUSER_OS:-}" ]; then
	PGUSER_OS="${SUDO_USER:-}"
	[ "$PGUSER_OS" = root ] && PGUSER_OS=""
	[ -n "$PGUSER_OS" ] || PGUSER_OS="$(stat -c %U "$DIR")"
fi
[ -n "$PGUSER_OS" ] && [ "$PGUSER_OS" != root ] || { echo "set PGUSER_OS to a non-root user"; exit 1; }
PGBIN="${PGBIN:-$(ls -d /usr/lib/postgresql/*/bin 2>/dev/null | sort -V | tail -n 1)}"
[ -x "$PGBIN/pg_ctl" ] || { echo "no PostgreSQL found; set PGBIN (see docs/BUILD.md)"; exit 1; }
for t in pgbench psql initdb; do
	[ -x "$PGBIN/$t" ] || { echo "$PGBIN/$t is missing"; exit 1; }
done
PGDATA="${PGDATA:-/home/$PGUSER_OS/pgdata}"
PORT="${PORT:-5433}"
SOCK="${SOCK:-/tmp}"
DB="${DB:-bench}"
SCALE="${SCALE:-10}"
echo "user=$PGUSER_OS bin=$PGBIN data=$PGDATA port=$PORT"

as_pg() { setpriv --reuid=$PGUSER_OS --regid=$PGUSER_OS --init-groups env PATH="$PGBIN:$PATH" HOME=/home/$PGUSER_OS "$@"; }
P() { as_pg psql -X -q -h $SOCK -p $PORT -U $PGUSER_OS "$@"; }
step() { echo; echo "== $*"; }

# The VM's disk is small. The benchmark needs room.
free_mb=$(df -Pm "$(dirname "$PGDATA")" | awk 'NR==2 {print $4}')
[ "$free_mb" -ge 1000 ] || echo "WARNING: only ${free_mb} MB free under $(dirname "$PGDATA"); runs may fail with 'No space left on device'"

# ---- 1. cgroups ----
step "cgroups"
"$DIR/pgext/setup_cgroups.sh" setup $PGUSER_OS

# ---- 2. PostgreSQL, started inside ufs-pg/main ----
step "PostgreSQL"
if [ ! -f "$PGDATA/PG_VERSION" ]; then
	echo "no cluster at $PGDATA, running initdb"
	as_pg initdb -D "$PGDATA" -U $PGUSER_OS
fi
if as_pg pg_ctl -D "$PGDATA" status >/dev/null 2>&1; then
	echo "already running; restarting so it lands in ufs-pg/main"
	as_pg pg_ctl -D "$PGDATA" -m fast stop
fi
# The postmaster must start inside ufs-pg/main: this shell enters that cgroup first
# (as root), then drops to the PostgreSQL user. Children inherit the cgroup.
bash -c 'echo $$ > /sys/fs/cgroup/ufs-pg/main/cgroup.procs; exec "$@"' _ \
	setpriv --reuid=$PGUSER_OS --regid=$PGUSER_OS --init-groups \
	env PATH="$PGBIN:$PATH" HOME=/home/$PGUSER_OS \
	pg_ctl -D "$PGDATA" -o "-p $PORT -k $SOCK" -l /tmp/pg.log -w start
CG="$(cat /proc/$(head -1 $PGDATA/postmaster.pid)/cgroup)"
echo "postmaster cgroup: $CG"
[ "$CG" = "0::/ufs-pg/main" ] || { echo "ERROR: postmaster is not in ufs-pg/main"; exit 1; }

# ---- 3. config, roles, database ----
step "roles, database, config"
P postgres -c "ALTER SYSTEM SET session_preload_libraries = 'pgtier';" \
           -c "ALTER SYSTEM SET autovacuum = off;" \
           -c "SELECT pg_reload_conf();" >/dev/null
role() {	# role NAME: create if missing
	P postgres -tAc "SELECT 1 FROM pg_roles WHERE rolname='$1'" | grep -q 1 \
		|| P postgres -c "CREATE ROLE $1 LOGIN;"
}
role oltp_app
role report_user
P postgres -tAc "SELECT 1 FROM pg_database WHERE datname='$DB'" | grep -q 1 \
	|| P postgres -c "CREATE DATABASE $DB;"
P $DB -c "GRANT ALL ON SCHEMA public TO oltp_app, report_user;"
P postgres -c "ALTER ROLE oltp_app SET pgtier.tier = 'ts';" \
           -c "ALTER ROLE report_user SET pgtier.tier = 'bg';"

# ---- 4. pgbench data ----
step "pgbench data"
if P $DB -tAc "SELECT count(*) FROM pgbench_accounts" 2>/dev/null | grep -q '^[1-9]'; then
	echo "pgbench tables already loaded"
else
	as_pg pgbench -i -s $SCALE -h $SOCK -p $PORT -U $PGUSER_OS $DB
fi
P $DB -c "GRANT ALL ON ALL TABLES IN SCHEMA public TO oltp_app, report_user;" \
      -c "VACUUM ANALYZE;"

# ---- 5. check that a ts backend really moves ----
step "tier check"
as_pg psql -X -h $SOCK -p $PORT -U oltp_app $DB -c "SHOW pgtier.tier;" 2>&1 | sed 's/^/  /'

# ---- 6. scheduler ----
step "scheduler"
if pgrep -x scx_pgtier.run >/dev/null || pgrep -x scx_pgtier >/dev/null; then
	echo "already running: $(pgrep -a scx_pgtier | head -n 1)"
elif [ $START_SCHED -eq 1 ]; then
	[ -x "$DIR/build/scx_pgtier" ] || { echo "build first: see docs/BUILD.md"; exit 1; }
	# Run a private copy: a binary that was just rebuilt on the host can be stale in
	# the VM's file cache and crash at start-up.
	sync; echo 3 > /proc/sys/vm/drop_caches
	cp "$DIR/build/scx_pgtier" /tmp/scx_pgtier.run
	setsid /tmp/scx_pgtier.run > /tmp/sched.log 2>&1 < /dev/null &
	sleep 3
	pgrep -a scx_pgtier || { echo "scheduler did not start:"; tail -n 5 /tmp/sched.log; exit 1; }
	echo "sched_ext state: $(cat /sys/kernel/sched_ext/state)"
else
	echo "skipped (--no-sched)"
fi

echo
echo "ready. Next: see docs/BENCHMARK.md to run the benchmark."
