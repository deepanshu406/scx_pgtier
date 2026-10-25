#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# One-time cgroup setup for the pgtier extension. Run as root.
#
#   ./setup_cgroups.sh setup <pg-user>   create and delegate the cgroups
#   ./setup_cgroups.sh remove            remove them (they must hold no processes)
#
# Creates this tree and gives it to <pg-user> (the user PostgreSQL runs as), so
# a PostgreSQL backend can move itself without being root:
#
#   /sys/fs/cgroup/ufs-pg/               delegated parent, cpu controller on
#   /sys/fs/cgroup/ufs-pg/main/          where the postmaster is started
#   /sys/fs/cgroup/ufs-pg/ufs-ts-pg/     time-sensitive backends
#   /sys/fs/cgroup/ufs-pg/ufs-bg-pg/     background backends
#
# Why "main": with the cpu controller enabled for its children, ufs-pg may not
# hold processes itself (the "no internal process" rule of cgroup v2). Also, a
# user may only move a process between two cgroups when it can write the
# cgroup.procs of their closest common ancestor, which is ufs-pg here. The
# postmaster therefore has to START inside ufs-pg/main (see the hint printed at
# the end).
set -eu

CG=/sys/fs/cgroup
BASE="$CG/ufs-pg"
DIRS="main ufs-ts-pg ufs-bg-pg"

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ]            || die "run as root"
[ -f "$CG/cgroup.controllers" ] || die "cgroup v2 is not mounted at $CG"

case "${1:-}" in
setup)
	USER_NAME="${2:-}"
	[ -n "$USER_NAME" ]           || die "usage: $0 setup <pg-user>"
	id "$USER_NAME" >/dev/null 2>&1 || die "no such user: $USER_NAME"

	# The cpu controller must be on all the way down.
	grep -qw cpu "$CG/cgroup.controllers" || die "the cpu controller is not available"
	grep -qw cpu "$CG/cgroup.subtree_control" || echo +cpu > "$CG/cgroup.subtree_control"

	mkdir -p "$BASE"
	for d in $DIRS; do mkdir -p "$BASE/$d"; done
	grep -qw cpu "$BASE/cgroup.subtree_control" || echo +cpu > "$BASE/cgroup.subtree_control"

	# Delegation: the directory itself plus the files the user must write.
	chown "$USER_NAME" "$BASE" "$BASE/cgroup.procs" "$BASE/cgroup.threads" \
		"$BASE/cgroup.subtree_control"
	for d in $DIRS; do
		chown "$USER_NAME" "$BASE/$d" "$BASE/$d/cgroup.procs" "$BASE/$d/cgroup.threads"
	done

	echo "done. cgroups under $BASE are delegated to $USER_NAME:"
	ls -ld "$BASE" "$BASE"/*/ 
	echo
	echo "Start the postmaster inside ufs-pg/main, for example:"
	echo "  sh -c 'echo \$\$ > $BASE/main/cgroup.procs; exec su $USER_NAME -c \"pg_ctl ... start\"'"
	;;
remove)
	for d in $DIRS; do
		[ -d "$BASE/$d" ] && rmdir "$BASE/$d"
	done
	[ -d "$BASE" ] && rmdir "$BASE"
	echo "removed $BASE"
	;;
*)
	die "usage: $0 setup <pg-user> | remove"
	;;
esac
