# scx_pgtier

A two-tier, deliberately *unfair* [sched_ext](https://github.com/sched-ext/scx) (eBPF) CPU scheduler
for PostgreSQL servers that run short transactions next to long-running queries.

**It is built for mixed workloads.** It trades some background throughput for lower latency of the
time-sensitive work. It is not meant for a server that only runs one kind of work.

> **Status: early and experimental.** Tested only in a virtual machine, on one synthetic workload.

## Where this comes from

This is an independent, open-source implementation of the idea described in

> C.-E. Bilodeau-Savaria, J. K. Nidzwetzki, S. Scherzinger, B. Kemme.
> *Unfair by design: eBPF-based scheduling of mixed database workloads.*
> [arXiv:2605.02377](https://arxiv.org/abs/2605.02377)

I am not affiliated with the authors and I did not have access to their code. The paper does not
include a code-availability statement, so everything here was written from the paper's description.
Credit for the idea goes to them. Mistakes in this implementation are mine.

Parts of the code are derived from `scx_simple` (Copyright Meta Platforms, Inc.; Tejun Heo,
David Vernet). Their notices are kept in the source files.

## What it does

PostgreSQL work is split into two tiers by **cgroup name**:

- **time-sensitive** tier (`ufs-ts-*`): short transactions. These run first.
- **background** tier (`ufs-bg-*`): long reports and maintenance. These share whatever CPU is left,
  fairly between cgroups.

A small PostgreSQL extension (`pgext/`) puts each backend into the cgroup of its tier, based on its
role. The scheduler is *selectively unfair*: a time-sensitive task that wakes up can take the CPU from a
background task at once.

### Priority inversion (locks held by background tasks)

Being unfair has a risk: a background query can hold a row lock while time-sensitive tasks keep every
CPU busy, so it never runs, and a time-sensitive transaction that needs that lock waits for as long as
the load lasts. Two mechanisms limit this:

- **Aging.** If waiting background tasks have not run for `age_ns` (50 ms), the scheduler lets one run.
- **Lock-holder boost.** With `-d CONNINFO` the loader asks PostgreSQL every 20 ms which backends block
  other backends (`pg_blocking_pids`). Blocking *background* backends are treated as time-sensitive
  until they release the lock.

## Results

Setup: 8-vCPU VM on an AMD Ryzen 9 9955HX host, kernel 7.0.0-38-generic, PostgreSQL 16, `pgbench` with
8 clients plus 8 long report queries running in a loop, 30 s per run, scheduler on and off alternated,
median of 5 runs after one discarded warm-up pair, lock-holder boost on (`-d`). Repeating the whole set
gives results that differ by a few percentage points.

| Mixed workload, full speed | scx_pgtier | EEVDF (stock) | change |
|---|---|---|---|
| transactions per second | 16,339 | 14,005 | +17% |
| p99 latency | 0.95 ms | 2.01 ms | -53% |
| p99.9 latency | 2.9 ms | 6.3 ms | -54% |
| report queries completed | 160 | 188 | -15% |
| report queries completed, same transaction rate (12k tps) | 183 | 189 | -3% |

| Solo (no report queries) | scx_pgtier | EEVDF (stock) | change |
|---|---|---|---|
| transactions per second | 20,078 | 26,528 | -24% |
| p99 latency | 0.65 ms | 0.52 ms | +25% |

| Wait for a row lock held by a background query | scx_pgtier | EEVDF (stock) |
|---|---|---|
| without aging and boost | about 25 s (as long as the load) | 0.5 s |
| with aging and boost | 1.85 to 2.44 s (30 runs, no stall in 45) | 0.93 to 0.96 s |

The lock test: the background holder needs about 1.3 s of CPU, 16 time-sensitive sessions keep the 8
CPUs busy, and a time-sensitive transaction then updates the locked row. The two rows used different head
starts for the holder (2 s and 0.3 s), so compare each row only within itself. See
[docs/BENCHMARK.md](docs/BENCHMARK.md).

- **The time-sensitive tier gains at the background tier's expense.** About 3% fewer report queries
  complete at equal transaction work, 15% fewer at full speed.
- **A lock held by a background task is now a bounded wait,** about twice what EEVDF gives in the same
  test, instead of a wait as long as the load lasts.
- **Maximum latency is not improved.** The slowest single transaction in a run is longer with
  scx_pgtier (median 42 ms against 21 ms), and in 2 of 5 runs one transaction took 0.76 s and 0.95 s.
  It is one event per run and noisy, but it is not better.
- **Limits of the lock handling.** It covers row and table locks held by background backends.
  Spinlocks and LWLocks are not covered (aging only). A holder is boosted only once someone waits for it,
  so it can lose some progress before that. The boost needs a database connection (`-d`).

How to reproduce these numbers: [docs/BENCHMARK.md](docs/BENCHMARK.md).

## Build and run

See [docs/BUILD.md](docs/BUILD.md). Requires a kernel with sched_ext.

## License

GPL-2.0, see [LICENSE](LICENSE).
