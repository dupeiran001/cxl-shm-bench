# synchronization — reproducing Figure 5 and Figure 6

Reproduces Figure 5 (scalability of cross-host locks vs number of hosts)
and Figure 6 (a)–(d) (lock latency vs locks + hosts, per algorithm).

All measurements are taken against **remote CXL** memory (CXL devices
reached through the memory switch). Both input CSVs in `../result/raw/`
are collected by `run.sh`, which SSH-fans-out `lock_bench` invocations
to four hosts via `run_commands.py`. The plot script consumes those
CSVs directly.

## Layout

```
synchronization/
├── lock_bench.c, *_lock.{c,h}, Makefile    ← binary source
├── scripts/ run.sh              — top-level runner
│            run_commands.py     — SSH fan-out worker
│            run_all*.json       — command plans for the two sweeps
│            plot_lock_figs.py   — parse runlogs → figures
│            README.md           — this file
└── result/
    ├── raw/     run_all.csv, run_all_central.csv
    └── plots/   1_latency.{pdf,png}              (Figure 5)
                 2_latency_{lfm,peterson,bakery,centralized}.{pdf,png}  (Figure 6 a-d)
```

## Prerequisites

- **4 hosts** reachable by passwordless SSH, all with the repo cloned at
  the same path and `lock_bench` built from `synchronization/`.
- Shared CXL region accessible on all 4 hosts via `/dev/interleave_dax`
  (Artifact A1 loaded on each host).
- Python ≥ 3.9 on the host running `run.sh` / `plot_lock_figs.py`,
  with `pandas`, `numpy`, `matplotlib`.

## Build the benchmark binary

```sh
# from synchronization/ (all four hosts need this)
make
```

`lock_bench` is a self-contained C binary — no external deps beyond
pthreads. Each host gets its own copy via the shared repo clone.

## Collect raw data (Figure 5 + Figure 6)

```sh
export HOSTS="root@hostA,root@hostB,root@hostC,root@hostD"
./run.sh
```

The JSON plans reference `lock_bench` via a `{BIN_ROOT}` placeholder that
`run.sh` substitutes before invoking `run_commands.py`. By default
`BIN_ROOT` is `$(cd scripts/.. && pwd)` on the controller — which
assumes every remote host has the repo checked out at the same absolute
path. If your remote layout differs, export `BIN_ROOT` to the remote
`synchronization/` directory (holding `lock_bench`) before running:

```sh
export BIN_ROOT=/opt/cxl_shm_profiling/synchronization
./run.sh
```

This issues two SSH-fanout sweeps through `run_commands.py`:

| Sweep | Plan | Output CSV | What it measures |
|---|---|---|---|
| shared CXL, decentralized | `run_all.json` | `../result/raw/run_all.csv` | LFM / Peterson / Bakery across 1–4 hosts × 1,2,4,8 locks over the multi-host interleaved region, plus the single-host Spin thread-sweep baseline |
| shared CXL, centralized | `run_all_centralized.json` | `../result/raw/run_all_central.csv` | Centralized server (host 0) + N-1 clients across 1–4 hosts × 1,2,4,8 locks |

The two plans hold exactly the configurations the figures consume
(threads=1, `hold_ns=0`, `gap_ns=0`); the sweeps together take roughly
~10–15 min with the default `--seconds 10` per point. `run.sh` calls
`run_commands.py --direct` which issues the
commands straight over SSH (no `bash -lc` wrapping); results from all
four hosts are interleaved into a single runlog CSV with columns
`timestamp_utc, group, cmd_index, host, command, exit_code,
duration_s, stdout, stderr`.

## Generate figures

```sh
python3 plot_lock_figs.py
```

Reads the 2 CSVs from `../result/raw/` and writes `1_latency.{pdf,png}` +
`2_latency_{lfm,peterson,bakery,centralized}.{pdf,png}` into
`../result/plots/`. Also prints a per-lock summary table to stdout (median
`lock_avg_ns` by `(lock_algorithm × hosts × locks)`).

The script intentionally fails loudly (`sys.exit(1)`) on any
non-zero exit code or missing required data in the input CSVs, so you
see immediately if a sweep had a host drop out.

## Smaller debugging runs

To verify a single host/algorithm before committing to the full
sweep, call `lock_bench` directly on each host:

```sh
# host 0 (initializer, must start first)
./lock_bench --lock lfm --hosts 2 --host_id 0 --threads 1 \
    --locks 1 --hold_ns 0 --gap_ns 0 --seconds 10 --init --no_flush_stats

# host 1 (starts once host 0 has written the shared init marker)
./lock_bench --lock lfm --hosts 2 --host_id 1 --threads 1 \
    --locks 1 --hold_ns 0 --gap_ns 0 --seconds 10 --no_flush_stats
```

`lock_bench --help` lists all flags (lock algorithm, concurrency,
critical-section hold time, post-release gap, sharing, DAX device,
etc.). `run.sh` just composes these invocations in the right shape
for the paper.
