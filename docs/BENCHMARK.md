# Benchmark

How the numbers in the [README](../README.md) were measured, and how to repeat them.

Results are from commit `eeb7ed7`, in a virtme-ng VM with 8 vCPUs and 8 GB of memory, on PostgreSQL 16.
"EEVDF" means the kernel's default scheduler, with no sched_ext scheduler loaded.

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

`compare` starts and stops the scheduler itself, so do not start it by hand. A quick check that
everything works takes seconds: `SECS=10 compare 1 mixed ""`. Do not use those numbers, because
10-second runs have a much noisier p99.9.

## Results

Medians of 5 alternating pairs.

**Mixed workload, full speed**

| | scx_pgtier | EEVDF | change |
|---|---|---|---|
| transactions per second | 16,682 | 14,358 | +16% |
| p50 latency | 412 us | 480 us | -14% |
| p99 latency | 904 us | 2,057 us | -56% |
| p99.9 latency | 2,989 us | 6,423 us | -53% |
| max latency | 44.7 ms | 21.0 ms | +113% |
| report queries completed | 160 | 187 | -14% |

**Solo (no report queries)**

| | scx_pgtier | EEVDF | change |
|---|---|---|---|
| transactions per second | 19,810 | 25,885 | -23% |
| p50 latency | 392 us | 297 us | +32% |
| p99 latency | 655 us | 530 us | +24% |
| p99.9 latency | 819 us | 668 us | +23% |
| max latency | 4.7 ms | 2.3 ms | +106% |

**Mixed at a fixed rate (`-R 12000`)**

| | scx_pgtier | EEVDF | change |
|---|---|---|---|
| transactions per second | 11,997 | 11,994 | +0% |
| report queries completed | 163 | 175 | -7% |

## How to read them

- The scheduler does what it is built for. With report queries running it delivers more
  transactions per second and a shorter tail (p99 and p99.9 about half).
- The background work pays for it: 7% fewer report queries complete at equal transaction
  work, 14% fewer at full speed.
- With nothing to protect it is slower (23% fewer transactions per second, higher latency).
  It is meant for servers that run both kinds of work.

## Caveats

- **Maximum latency is not better.** It is a single event per run and noisy, but in both the
  mixed and the solo runs the median maximum is higher with scx_pgtier.
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
- **One setup:** a VM with 8 vCPUs, one synthetic workload, one kernel, one PostgreSQL version.
