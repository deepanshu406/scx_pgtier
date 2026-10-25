/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pgtier: PostgreSQL side of scx_pgtier.
 *
 * Step 4 of the extension: pgtier.tier moves the backend into a cgroup and
 * switches it to SCHED_EXT, so scx_pgtier schedules it.
 *
 *   pgtier.tier = 'none'   <cgroup_root>/main        normal scheduling (default)
 *               = 'ts'     <cgroup_root>/ufs-ts-pg   SCHED_EXT, time-sensitive
 *               = 'bg'     <cgroup_root>/ufs-bg-pg   SCHED_EXT, background
 *
 * Order of actions on every change (the scheduler reads a task's cgroup name
 * once, when the task joins sched_ext, so the cgroup must change while the
 * task is outside it):
 *   1. if the backend is in SCHED_EXT, switch back to SCHED_OTHER
 *   2. move it to the cgroup of the new tier
 *   3. if the new tier is not 'none', switch to SCHED_EXT
 *
 *   pgtier.cgroup_root     directory made by setup_cgroups.sh,
 *                          default /sys/fs/cgroup/ufs-pg
 *
 * The scheduler decides the tier from the cgroup NAME (ufs-ts-*, ufs-bg-*).
 *
 * Both settings are superuser-only. A superuser can fix them for a role or a
 * database with ALTER ROLE ... SET, and can allow a trusted role to change
 * pgtier.tier with GRANT SET ON PARAMETER pgtier.tier TO some_role.
 *
 * The postmaster must be started inside <cgroup_root>/main (see
 * setup_cgroups.sh) so that an unprivileged backend may move itself between
 * the cgroups below <cgroup_root>. Use session_preload_libraries = 'pgtier'.
 *
 * If a move fails, the hook only logs a warning. It never stops a query.
 */
#include "postgres.h"

#include <fcntl.h>
#include <sched.h>
#include <unistd.h>

#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

/* Older libc headers do not define SCHED_EXT. */
#ifndef SCHED_EXT
#define SCHED_EXT 7
#endif

void		_PG_init(void);

enum
{
	TIER_NONE = 0,
	TIER_TS = 1,
	TIER_BG = 2
};

/* The values the setting accepts. Anything else is rejected with an error. */
static const struct config_enum_entry tier_options[] = {
	{"none", TIER_NONE, false},
	{"ts", TIER_TS, false},
	{"bg", TIER_BG, false},
	{NULL, 0, false}
};

/* The current values for this backend. PostgreSQL keeps them up to date. */
static int	pgtier_tier = TIER_NONE;
static char *pgtier_cgroup_root = NULL;

/*
 * Where this backend is now. A new backend starts in main under SCHED_OTHER.
 * cg_tier is the tier whose cgroup it is in; in_ext tells if it is in SCHED_EXT.
 */
static int	cg_tier = TIER_NONE;
static bool in_ext = false;

static const char *
tier_name(int tier)
{
	switch (tier)
	{
		case TIER_TS:
			return "ts";
		case TIER_BG:
			return "bg";
		default:
			return "none";
	}
}

/* The cgroup directory name, under cgroup_root, for a tier. */
static const char *
tier_cgroup(int tier)
{
	switch (tier)
	{
		case TIER_TS:
			return "ufs-ts-pg";
		case TIER_BG:
			return "ufs-bg-pg";
		default:
			return "main";
	}
}

/*
 * Write this backend's pid into <cgroup_root>/<subdir>/cgroup.procs.
 * Returns true on success. On failure it logs a WARNING and returns false; it
 * must never raise an ERROR, because it runs inside a GUC assign hook.
 */
static bool
move_to_cgroup(const char *subdir)
{
	char		path[MAXPGPATH];
	char		pid[32];
	int			len;
	int			fd;
	int			saved_errno;
	ssize_t		written;

	len = snprintf(path, sizeof(path), "%s/%s/cgroup.procs",
				   pgtier_cgroup_root ? pgtier_cgroup_root : "", subdir);
	if (len < 0 || len >= (int) sizeof(path))
	{
		ereport(WARNING,
				(errmsg("pgtier: cgroup path is too long")));
		return false;
	}

	fd = open(path, O_WRONLY);
	if (fd < 0)
	{
		ereport(WARNING,
				(errmsg("pgtier: could not open \"%s\": %m", path)));
		return false;
	}

	len = snprintf(pid, sizeof(pid), "%d", MyProcPid);
	written = write(fd, pid, len);
	saved_errno = errno;
	close(fd);

	if (written != len)
	{
		errno = saved_errno;
		ereport(WARNING,
				(errmsg("pgtier: could not move pid %d to \"%s\": %m",
						MyProcPid, path)));
		return false;
	}
	return true;
}

/*
 * Switch this backend to the given scheduling policy. Returns true on success.
 * On failure it logs a WARNING and returns false; it never raises an ERROR.
 */
static bool
set_policy(int policy)
{
	struct sched_param param;

	memset(&param, 0, sizeof(param));
	if (sched_setscheduler(0, policy, &param) != 0)
	{
		ereport(WARNING,
				(errmsg("pgtier: could not switch pid %d to policy %d: %m",
						MyProcPid, policy)));
		return false;
	}
	return true;
}

/*
 * Called every time pgtier.tier changes: by SET, RESET, SET LOCAL ending, a
 * rolled-back transaction, a parallel worker restoring the leader's settings,
 * or a session starting with a value from ALTER ROLE. It must not fail.
 *
 * Each step is done only if the previous one worked. If something fails, the
 * backend stays where it got to, and the next change tries again.
 */
static void
pgtier_tier_assign(int newval, void *extra)
{
	if (newval == cg_tier && in_ext == (newval != TIER_NONE))
		return;					/* already there; also covers the startup call */

	/* 1. leave the BPF scheduler, so the cgroup can change safely */
	if (in_ext)
	{
		if (!set_policy(SCHED_OTHER))
			return;
		in_ext = false;
	}

	/* 2. move to the cgroup of the new tier */
	if (cg_tier != newval)
	{
		if (!move_to_cgroup(tier_cgroup(newval)))
			return;
		cg_tier = newval;
	}

	/* 3. join the BPF scheduler; it reads the cgroup name now */
	if (newval != TIER_NONE)
	{
		if (!set_policy(SCHED_EXT))
			return;
		in_ext = true;
	}

	elog(LOG, "pgtier: backend pid=%d is now tier %s (cgroup %s, sched_ext %s)",
		 MyProcPid, tier_name(newval), tier_cgroup(newval), in_ext ? "on" : "off");
}

/* Read the cgroup this process is in from /proc/self/cgroup, e.g. "0::/ufs-pg/main". */
static void
current_cgroup(char *buf, size_t size)
{
	int			fd;
	ssize_t		n;
	char	   *nl;

	snprintf(buf, size, "?");
	fd = open("/proc/self/cgroup", O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n <= 0)
	{
		snprintf(buf, size, "?");
		return;
	}
	buf[n] = '\0';
	nl = strchr(buf, '\n');
	if (nl)
		*nl = '\0';
}

/* Called once, when the library is loaded into this backend. */
void
_PG_init(void)
{
	DefineCustomStringVariable("pgtier.cgroup_root",
							   "Directory that holds the pgtier cgroups.",
							   "Made by setup_cgroups.sh; contains main, ufs-ts-pg and ufs-bg-pg.",
							   &pgtier_cgroup_root,
							   "/sys/fs/cgroup/ufs-pg",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);

	DefineCustomEnumVariable("pgtier.tier",
							 "Scheduler tier of this backend.",
							 "none leaves the backend alone, ts is time-sensitive, bg is background.",
							 &pgtier_tier,
							 TIER_NONE,
							 tier_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 pgtier_tier_assign,
							 NULL);

	/* No other extension may define settings that start with "pgtier.". */
	MarkGUCPrefixReserved("pgtier");

	elog(LOG, "pgtier: loaded in backend pid=%d", MyProcPid);
}

PG_FUNCTION_INFO_V1(pgtier_hello);

/* SELECT pgtier_hello(); shows the tier, the scheduling policy (0 normal, 7 sched_ext) and the cgroup. */
Datum
pgtier_hello(PG_FUNCTION_ARGS)
{
	char		cg[128];
	char		buf[320];

	current_cgroup(cg, sizeof(cg));
	snprintf(buf, sizeof(buf), "pgtier step 4, backend pid %d, tier %s, policy %d, cgroup %s",
			 MyProcPid, tier_name(pgtier_tier), sched_getscheduler(0), cg);
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}
