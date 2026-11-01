# Benchmark

How the numbers in the [README](../README.md) were measured, and how to repeat them.

Results are from commit `2b2a206`, in a virtme-ng VM with 8 vCPUs and 8 GB of memory, on PostgreSQL 16.
The host CPU is an AMD Ryzen 9 9955HX, and the kernel is `7.0.0-38-generic`.
"EEVDF" means the kernel's default scheduler, with no sched_ext scheduler loaded. "scx_pgtier" runs
with aging (`age_ns` = 50 ms) and with the lock-holder boost on (`-d`, see below).

## What is measured

Two kinds of work run on the same 8 CPUs:

- **Time-sensitive:** `pgbench` with 8 clients (`-c 8 -j 4`, default TPC-B-like transactions, scale 10,
  `-n`), connecting as role `oltp_app`, which is in the `ts` tier.
- **Background:** 8 loops that each run one CPU-only report query after another, as role
  `report_user`, which is in the `bg` tier. The query uses only CPU, no disk:
  ```sql
  SELECT sum(sqrt(i::float8)) FROM (SELECT generate_series(1,60000000) AS i) s;
  ```

The report loops start 5 s before `pgbench`, so the machine is already full when measurement starts.
`pgbench` runs for 30 s and writes a per-transaction log; the percentiles come from that log
(`pct` in `scripts/bench_funcs.sh`). The report queries are counted for as long as the loops run
(45 s: the 5 s head start, the 30 s run, and the remaining time).

Two other modes use the same machinery:

- **solo:** `pgbench` alone, no report queries. What the scheduler costs when there is nothing to protect.
- **fixed rate:** the mixed workload with `pgbench -R 12000`. Both schedulers deliver the same
  transaction rate, so the number of report queries completed shows what the background work pays.

## Method

- **Alternating runs.** One run with the scheduler, then one without, five times. Both
  conditions see the same slow drift of the machine.
- **One discarded warm-up pair** before each set. The first pair behaves differently from the rest.
- **Median** of the five runs for every column.
- **Same starting state.** Before every run `pgbench_history` is truncated and the small tables are
  vacuumed (`VACUUM FULL`). Autovacuum is off, and without this the branch and teller tables bloat
  over a session and later runs get slower than earlier ones.
- **A private copy of the scheduler binary** is started after dropping the page cache, so a binary
  that was just rebuilt on the host cannot be stale in the VM.

## Repeat it

Set up the VM as in [BUILD.md](BUILD.md#clean-vm-from-scratch), then inside the VM, as root, from the
repository root:

```
scripts/vm_setup.sh --no-sched
source scripts/bench_funcs.sh
export SCX_DB="host=$SOCK port=$PORT dbname=$DB user=$ADMIN"   # lock-holder boost on (optional)

# 1. mixed, full speed (about 10 minutes)
compare 1 mixed "" > /dev/null 2>&1                 # warm-up pair, discarded
compare 5 mixed "" 2>&1 | tee /tmp/step1_mixed.txt
summarize /tmp/step1_mixed.txt

# 2. solo (about 6 minutes)
compare 1 solo "" > /dev/null 2>&1
compare 5 solo "" 2>&1 | tee /tmp/step2_solo.txt
summarize /tmp/step2_solo.txt

# 3. fixed rate (about 10 minutes)
compare 1 mixed "-R 12000" > /dev/null 2>&1
compare 5 mixed "-R 12000" 2>&1 | tee /tmp/step3_rate.txt
summarize /tmp/step3_rate.txt
```

`compare` starts and stops the scheduler itself, so do not start it by hand. If `SCX_DB` is set,
`sched_on` passes it to the scheduler as `-d "$SCX_DB"`, which turns the lock-holder boost on. A quick check that
everything works takes seconds: `SECS=10 compare 1 mixed ""`. Do not use those numbers, because
10-second runs have a much noisier p99.9.

## Results

Medians of 5 alternating pairs.

**Mixed workload, full speed**

| | scx_pgtier | EEVDF | change |
|---|---|---|---|
| transactions per second | 16,339 | 14,005 | +17% |
| p50 latency | 438 us | 489 us | -10% |
| p99 latency | 952 us | 2,007 us | -53% |
| p99.9 latency | 2,888 us | 6,251 us | -54% |
| max latency | 41.6 ms | 21.0 ms | +98% |
| report queries completed | 160 | 188 | -15% |

**Solo (no report queries)**

| | scx_pgtier | EEVDF | change |
|---|---|---|---|
| transactions per second | 20,078 | 26,528 | -24% |
| p50 latency | 389 us | 290 us | +34% |
| p99 latency | 650 us | 518 us | +25% |
| p99.9 latency | 815 us | 651 us | +25% |
| max latency | 4.0 ms | 2.7 ms | +51% |

**Mixed at a fixed rate (`-R 12000`)**

| | scx_pgtier | EEVDF | change |
|---|---|---|---|
| transactions per second | 11,995 | 11,989 | +0% |
| report queries completed | 183 | 189 | -3% |

### Lock held by a background query

`tests/test_lock_holder.sh` measures how long a time-sensitive transaction waits for a row lock that a
background query holds. The background query takes the lock and then needs about 1.3 s of CPU (alone on
an idle machine). 16 time-sensitive sessions keep the 8 CPUs busy. The transaction starts 0.3 s after the
load starts.

| | wait for the lock |
|---|---|
| EEVDF | 0.93 to 0.96 s (3 runs) |
| scx_pgtier, aging and boost | 1.85 to 2.44 s (30 runs; no stall in 45 runs) |
| scx_pgtier before aging and boost | 25.7 s, as long as the load lasts (3 runs, with a 2 s head start; EEVDF 0.5 s in the same test) |

Run it as root in the VM, from the repository root: `tests/test_lock_holder.sh quick` (one pair), or
`tests/test_lock_holder.sh 5` (warm-up pair, then 5 alternating pairs).

## How to read them

- The scheduler does what it is built for. With report queries running it delivers more
  transactions per second and a shorter tail (p99 and p99.9 about half).
- The background work pays for it: 3% fewer report queries complete at equal transaction
  work, 15% fewer at full speed.
- With nothing to protect it is slower (24% fewer transactions per second, higher latency).
  It is meant for servers that run both kinds of work.
- A lock held by a background query is a bounded wait of about 2 s here, about twice what EEVDF gives
  in the same test. Without aging and boost it lasted as long as the load.

## Caveats

- **Maximum latency is not better.** It is a single event per run and noisy, but in both the
  mixed and the solo runs the median maximum is higher with scx_pgtier. In the final mixed set,
  2 of the 5 scx_pgtier runs had one transaction of 0.76 s and 0.95 s, which the median hides.
- **Results move between sets.** An earlier complete set of the mixed run, with the same code, gave
  +21% transactions per second, -51% p99 and -63% p99.9. Across the two sets the gains were
  +16% to +21% (transactions per second), -51% to -56% (p99) and -53% to -63% (p99.9). Read them as
  ranges, not exact values.
- **Absolute numbers drift** within and between sessions (they changed by several thousand
  transactions per second during one session, for both schedulers). Compare the two conditions
  in the same set; do not compare raw numbers between sessions or machines.
- **Fixed-rate latency is not compared.** With `-R`, latency is measured from the time a transaction
  was scheduled to start, so any lag is included. Under this load EEVDF fell behind the schedule
  (median latency 3.6 ms, p99 108 ms), and scx_pgtier had a p99 of about 14 ms. I did not find out why,
  and a comparison of these numbers would not say what the scheduler does to a transaction. Only
  the transaction rate and the report-query count are used.
- **The lock handling has limits.** It covers row and table locks held by background backends.
  Spinlocks and LWLocks are not covered, only aging helps there. A holder is boosted only when someone
  waits for it (the loader polls every 20 ms), so it can lose some progress before that. This is why
  the wait is about twice EEVDF's in the test above. Two causes of the remaining gap were not
  investigated: the boosted holder runs at about 85% of a CPU.
- **The lock test is one synthetic case.** One holder, one waiter, one hot row.
- **One setup:** a VM with 8 vCPUs, one synthetic workload, one kernel, one PostgreSQL version.
