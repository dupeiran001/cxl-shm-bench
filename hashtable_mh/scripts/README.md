# hashtable_mh results — plotting

## Layout

```
hashtable_mh/
├── scripts/
│   ├── plot_ht.py          regenerate the figure from result/raw/hashtable.csv
│   └── README.md           this file
└── result/
    ├── raw/
    │   └── hashtable.csv      one row per (config, threads, mix, run)
    └── plots/
        └── fig_ht_mops_t<T>.{pdf,png}
```

`raw/hashtable.csv` schema:
`benchmark,hosts,threads_per_host,mix,run,mops,p50_ns,p95_ns,p99_ns`

- `benchmark` — configuration: `dram`, `remote_cxl`, `shared_2hosts`,
  `shared_4hosts`
- `mix` — read percentage of the workload (100 / 95 / 50)
- `run` — outer repetition index (RUNS in reproduce.sh)
- `mops` — aggregate cross-host throughput (from the host-0 `AGGREGATE`
  line)
- `p50_ns/p95_ns/p99_ns` — per-op latency percentiles of host 0's sampled
  operations

The CSV is produced by `../reproduce.sh`, which parses each point's
host-0 `AGGREGATE mops=... p50_ns=...` line and appends one row per run.

## Regenerating the figure

From this directory:

```sh
python3 plot_ht.py
```

Reads `../result/raw/hashtable.csv`, writes
`../result/plots/fig_ht_mops_t<threads>.{pdf,png}`: grouped bars (config × mix) of
mean aggregate Mops across runs, error bars = std across runs. Defaults to
the largest `threads_per_host` present. Useful flags:

- `--threads 16` — plot a different per-host thread count
- `--csv <path>` / `--out_dir <path>` — override the defaults
