# tool_memory_latency Scripts

Reproduces Figure 3 (a)–(c) of the paper: cache-miss load latency,
store→flush latency, and clean-cacheline flush latency for DRAM, local
CXL, and remote/shared CXL.

## Binary

Built from `benchmark/tool_memory_latency/memory_lat.cc` via the top-level
`benchmark/Makefile`:

```sh
make -C .. tool_memory_latency/tool_memory_latency
```

## Common parameters

- `--sz` — per-thread working set (exceeds LLC for `rd_miss`, fits in
  cache for `store_flush` / `flush_clean`)
- `--total_sz` — total bytes accessed
- `--lat_low` / `--lat_high` / `--lat_step` — histogram threshold sweep
  in CPU cycles
- `--flush={clwb|clflushopt}` — flush primitive for `store_flush` /
  `flush_clean`
- `--dax_dev=<path>` — backing DAX device. Omit for NUMA-backed DRAM;
  use a local DAX device (e.g. `/dev/dax0.0`) for local CXL;
  use `/dev/interleave_dax` for remote/shared CXL
- `-m <node>` / `-n <node>` — NUMA memory/CPU binding (DRAM runs only)

## Workflow: sweep, then extract percentiles

The tool does **not** emit p50/p95/p99 directly. It sweeps thresholds
`[lat_low .. lat_high]` with step `lat_step`, and for each threshold
logs one line of the form:

```
threshold 120 cycles: 47.3% <= threshold, avg 127.8 cycles
```

The percentage is monotonically non-decreasing as the threshold grows.
To produce the `<config>_<test>.csv` that the plot consumes, you
manually:

1. Run the tool with a broad sweep (e.g. `--lat_low=50 --lat_high=2000
   --lat_step=50`) and read the log lines.
2. Identify the thresholds at which the percentage first crosses 50%,
   95%, and 99% — those are `p50`, `p95`, `p99`.
3. If a step is too coarse to pin down a percentile, rerun with a
   narrower range and smaller step around that region.
4. Write a CSV in this format (example for `dram_store_flush.csv`):

```
measurement,value,method
avg,480,clwb
p50,500,clwb
p95,670,clwb
p99,760,clwb
avg,620,clflushopt
p50,630,clflushopt
p95,800,clflushopt
p99,990,clflushopt
```

`avg` is the `avg N cycles` reported by the tool (essentially constant
across the sweep for one run). For `store_flush` and `flush_clean`,
produce rows for both `clwb` and `clflushopt` under a single header.

CSVs must be named `<config>_<test>.csv` and land in `../../result/tool_memory_latency/raw/` so
`plot.sh` finds them:

- `<config> ∈ {dram, local_cxl, remote_cxl}`
- `<test> ∈ {rd_miss, store_flush, flush_clean}`

Each sweep below is a 5-point window centered on the target percentile
(from the CSVs in `../../result/tool_memory_latency/raw/`): `lat_low = T−2·step`, `lat_high = T+2·step`,
so the tool emits exactly five `threshold … cycles: X%` lines per run.
Read the lines and pick the one whose `X%` is nearest the target
percentile — its `threshold` is your value.

Run commands from `benchmark/tool_memory_latency/` (the binary lives
there).

## Reproducing the paper's CSVs

### Fig 3(a) — Cache-miss load latency (`rd_miss`)

Pointer-chased reads over a 1 GiB working set (≥ LLC to force LLC
misses).

#### DRAM — avg 111, p50 110, p95 151, p99 158

```bash
./tool_memory_latency --test=rd -m 0 -n 0 --sz=1G --total_sz=8G --lat_low=100 --lat_high=120 --lat_step=5  # p50
./tool_memory_latency --test=rd -m 0 -n 0 --sz=1G --total_sz=8G --lat_low=145 --lat_high=157 --lat_step=3  # p95
./tool_memory_latency --test=rd -m 0 -n 0 --sz=1G --total_sz=8G --lat_low=152 --lat_high=164 --lat_step=3  # p99
```

#### local CXL — avg 463, p50 510, p95 540, p99 1000

```bash
./tool_memory_latency --test=rd --dax_dev=/dev/dax0.0 --sz=1G --total_sz=8G --lat_low=470 --lat_high=550 --lat_step=20  # p50
./tool_memory_latency --test=rd --dax_dev=/dev/dax0.0 --sz=1G --total_sz=8G --lat_low=500 --lat_high=580 --lat_step=20  # p95
./tool_memory_latency --test=rd --dax_dev=/dev/dax0.0 --sz=1G --total_sz=8G --lat_low=900 --lat_high=1100 --lat_step=50 # p99
```

#### remote CXL — avg 990, p50 1070, p95 1295, p99 1440

```bash
./tool_memory_latency --test=rd --dax_dev=/dev/interleave_dax --sz=1G --total_sz=8G --lat_low=990 --lat_high=1150 --lat_step=40  # p50
./tool_memory_latency --test=rd --dax_dev=/dev/interleave_dax --sz=1G --total_sz=8G --lat_low=1195 --lat_high=1395 --lat_step=50 # p95
./tool_memory_latency --test=rd --dax_dev=/dev/interleave_dax --sz=1G --total_sz=8G --lat_low=1320 --lat_high=1560 --lat_step=60 # p99
```

### Fig 3(b) — Store → flush latency (`store_flush`)

Read a cache line (dirties it), then time store + flush + sfence. Run
once per flush primitive per config.

#### DRAM, clwb — avg 480, p50 500, p95 670, p99 760

```bash
./tool_memory_latency -m 0 -n 0 --test=store_flush --flush=clwb --sz=32M --total_sz=256M --lat_low=460 --lat_high=540 --lat_step=20  # p50
./tool_memory_latency -m 0 -n 0 --test=store_flush --flush=clwb --sz=32M --total_sz=256M --lat_low=630 --lat_high=710 --lat_step=20  # p95
./tool_memory_latency -m 0 -n 0 --test=store_flush --flush=clwb --sz=32M --total_sz=256M --lat_low=700 --lat_high=820 --lat_step=30  # p99
```

#### DRAM, clflushopt — avg 620, p50 630, p95 800, p99 990

```bash
./tool_memory_latency -m 0 -n 0 --test=store_flush --flush=clflushopt --sz=32M --total_sz=256M --lat_low=580 --lat_high=680 --lat_step=25  # p50
./tool_memory_latency -m 0 -n 0 --test=store_flush --flush=clflushopt --sz=32M --total_sz=256M --lat_low=740 --lat_high=860 --lat_step=30  # p95
./tool_memory_latency -m 0 -n 0 --test=store_flush --flush=clflushopt --sz=32M --total_sz=256M --lat_low=890 --lat_high=1090 --lat_step=50 # p99
```

#### local CXL, clwb — avg 706, p50 710, p95 800, p99 1100

```bash
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=store_flush --flush=clwb --sz=32M --total_sz=256M --lat_low=650 --lat_high=770 --lat_step=30   # p50
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=store_flush --flush=clwb --sz=32M --total_sz=256M --lat_low=740 --lat_high=860 --lat_step=30   # p95
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=store_flush --flush=clwb --sz=32M --total_sz=256M --lat_low=1000 --lat_high=1200 --lat_step=50 # p99
```

#### local CXL, clflushopt — avg 1084, p50 1070, p95 1170, p99 1560

```bash
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=store_flush --flush=clflushopt --sz=32M --total_sz=256M --lat_low=990 --lat_high=1150 --lat_step=40   # p50
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=store_flush --flush=clflushopt --sz=32M --total_sz=256M --lat_low=1090 --lat_high=1250 --lat_step=40  # p95
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=store_flush --flush=clflushopt --sz=32M --total_sz=256M --lat_low=1400 --lat_high=1720 --lat_step=80  # p99
```

#### remote CXL, clwb — avg 1331, p50 1300, p95 1450, p99 2300

```bash
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=store_flush --flush=clwb --sz=32M --total_sz=256M --lat_low=1200 --lat_high=1400 --lat_step=50   # p50
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=store_flush --flush=clwb --sz=32M --total_sz=256M --lat_low=1350 --lat_high=1550 --lat_step=50   # p95
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=store_flush --flush=clwb --sz=32M --total_sz=256M --lat_low=2100 --lat_high=2500 --lat_step=100  # p99
```

#### remote CXL, clflushopt — avg 2312, p50 2300, p95 2500, p99 2700

```bash
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=store_flush --flush=clflushopt --sz=32M --total_sz=256M --lat_low=2100 --lat_high=2500 --lat_step=100  # p50
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=store_flush --flush=clflushopt --sz=32M --total_sz=256M --lat_low=2300 --lat_high=2700 --lat_step=100  # p95
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=store_flush --flush=clflushopt --sz=32M --total_sz=256M --lat_low=2500 --lat_high=2900 --lat_step=100  # p99
```

### Fig 3(c) — Clean cacheline flush (`flush_clean`)

Flush a cache-resident (clean) line. Run once per flush primitive per
config.

#### DRAM, clwb — avg 72, p50 65, p95 180, p99 200

```bash
./tool_memory_latency -m 0 -n 0 --test=flush_clean --flush=clwb --sz=32M --total_sz=256M --lat_low=59 --lat_high=71 --lat_step=3    # p50
./tool_memory_latency -m 0 -n 0 --test=flush_clean --flush=clwb --sz=32M --total_sz=256M --lat_low=170 --lat_high=190 --lat_step=5  # p95
./tool_memory_latency -m 0 -n 0 --test=flush_clean --flush=clwb --sz=32M --total_sz=256M --lat_low=190 --lat_high=210 --lat_step=5  # p99
```

#### DRAM, clflushopt — avg 72, p50 65, p95 180, p99 210

```bash
./tool_memory_latency -m 0 -n 0 --test=flush_clean --flush=clflushopt --sz=32M --total_sz=256M --lat_low=59 --lat_high=71 --lat_step=3    # p50
./tool_memory_latency -m 0 -n 0 --test=flush_clean --flush=clflushopt --sz=32M --total_sz=256M --lat_low=170 --lat_high=190 --lat_step=5  # p95
./tool_memory_latency -m 0 -n 0 --test=flush_clean --flush=clflushopt --sz=32M --total_sz=256M --lat_low=200 --lat_high=220 --lat_step=5  # p99
```

#### local CXL, clwb — avg 79, p50 70, p95 170, p99 180

```bash
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=flush_clean --flush=clwb --sz=32M --total_sz=256M --lat_low=64 --lat_high=76 --lat_step=3    # p50
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=flush_clean --flush=clwb --sz=32M --total_sz=256M --lat_low=160 --lat_high=180 --lat_step=5  # p95
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=flush_clean --flush=clwb --sz=32M --total_sz=256M --lat_low=170 --lat_high=190 --lat_step=5  # p99
```

#### local CXL, clflushopt — avg 81, p50 60, p95 170, p99 210

```bash
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=flush_clean --flush=clflushopt --sz=32M --total_sz=256M --lat_low=54 --lat_high=66 --lat_step=3    # p50
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=flush_clean --flush=clflushopt --sz=32M --total_sz=256M --lat_low=160 --lat_high=180 --lat_step=5  # p95
./tool_memory_latency --dax_dev=/dev/dax0.0 --test=flush_clean --flush=clflushopt --sz=32M --total_sz=256M --lat_low=200 --lat_high=220 --lat_step=5  # p99
```

#### remote CXL, clwb — avg 60, p50 65, p95 160, p99 195

```bash
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=flush_clean --flush=clwb --sz=32M --total_sz=256M --lat_low=59 --lat_high=71 --lat_step=3    # p50
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=flush_clean --flush=clwb --sz=32M --total_sz=256M --lat_low=150 --lat_high=170 --lat_step=5  # p95
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=flush_clean --flush=clwb --sz=32M --total_sz=256M --lat_low=185 --lat_high=205 --lat_step=5  # p99
```

#### remote CXL, clflushopt — avg 62, p50 65, p95 170, p99 205

```bash
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=flush_clean --flush=clflushopt --sz=32M --total_sz=256M --lat_low=59 --lat_high=71 --lat_step=3    # p50
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=flush_clean --flush=clflushopt --sz=32M --total_sz=256M --lat_low=160 --lat_high=180 --lat_step=5  # p95
./tool_memory_latency --dax_dev=/dev/interleave_dax --test=flush_clean --flush=clflushopt --sz=32M --total_sz=256M --lat_low=195 --lat_high=215 --lat_step=5  # p99
```

## Plotting

```sh
./plot.sh
```

Reads the 9 CSVs from `../../result/tool_memory_latency/raw/`, writes Fig 3(a)–(c) as PDFs and PNGs to
`../../result/tool_memory_latency/plots/`.
