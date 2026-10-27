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

## Results

Setup: 8-vCPU VM, PostgreSQL 16, `pgbench` with 8 clients plus 8 long report queries running in a loop,
30 s per run, scheduler on and off alternated, median of 5 runs.

| Mixed workload, full speed | scx_pgtier | EEVDF (stock) | change |
|---|---|---|---|
| transactions per second | 17,405 | 14,979 | +16% |
| p99 latency | 0.92 ms | 1.89 ms | -51% |
| p99.9 latency | 2.3 ms | 5.9 ms | -61% |
| report queries completed | 160 | 188 | -15% |
| report queries completed, same transaction rate (12k tps) | 178 | 191 | -7% |

| Solo (no report queries) | scx_pgtier | EEVDF (stock) | change |
|---|---|---|---|
| transactions per second | 19,387 | 25,510 | -24% |

- **The time-sensitive tier gains at the background tier's expense.** About 7% fewer report queries
  complete at equal transaction work.

How to reproduce these numbers: [docs/BENCHMARK.md](docs/BENCHMARK.md).

## Build and run

See [docs/BUILD.md](docs/BUILD.md). Requires a kernel with sched_ext.

## License

GPL-2.0, see [LICENSE](LICENSE).
