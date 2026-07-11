# bw_mem Scripts

Drives `bw_mem` to reproduce Figure 2 (a)–(f) of the paper and Table I
configurations A–D. Raw CSVs land in `../../result/bw_mem/raw/`; `plot.sh` regenerates the
six paper figures into `../../result/bw_mem/plots/`.

## Common behavior

- Thread list is a comma-separated list (e.g. `1,2,4,8`).
- `--size` and `--dax_offset` accept `K/M/G/T` suffixes.
- DAX offsets must be page-aligned.
- CSV schema: `timestamp,mode,test,size_bytes,repetition,parallel,host,speed_mb_s`.

## Reproducing the paper's CSVs

Invocations used to produce each raw CSV. Adjust hostnames, DAX paths,
and NUMA nodes to your platform.

Figure 2 plots `rd` and `ntwr` only. Run both modes for each
configuration.

### Fig 2(a) — DRAM (Config A)

```bash
./run_dram_local.sh 1,2,4,8,16,32,64 --test rd   --size 1G --repetition 10 --cpu 0 --mem 0
./run_dram_local.sh 1,2,4,8,16,32,64 --test ntwr --size 1G --repetition 10 --cpu 0 --mem 0
```

### Fig 2(b) — 1 host, local CXL (Config B)

```bash
./run_dax_local.sh 1,2,4,8,16 --dax_dev /dev/dax0.0 --dax_offset 2G --test rd   --size 1G --repetition 10
./run_dax_local.sh 1,2,4,8,16 --dax_dev /dev/dax0.0 --dax_offset 2G --test ntwr --size 1G --repetition 10
```

### Fig 2(c) — 1 host, remote CXL (Config C)

Same invocation as (b), targeting a remote CXL DAX device through the
memory switch. The overlay series labeled `(4CXL)` in the plot is the
4-device software-interleaved variant:

```bash
# single remote device
./run_dax_local.sh 1,2,4,8,16 --dax_dev /dev/dax0.0           --dax_offset 2G --test rd   --size 1G --repetition 10
./run_dax_local.sh 1,2,4,8,16 --dax_dev /dev/dax0.0           --dax_offset 2G --test ntwr --size 1G --repetition 10

# 4 devices interleaved via interleave_dax
./run_dax_local.sh 1,2,4,8,16 --dax_dev /dev/interleave_dax  --dax_offset 2G --test rd   --size 1G --repetition 10
./run_dax_local.sh 1,2,4,8,16 --dax_dev /dev/interleave_dax  --dax_offset 2G --test ntwr --size 1G --repetition 10
```

### Fig 2(d) — 2 hosts, 1 CXL (Config D)

```bash
./run_dax_multi_host.sh 1,2,4,8,16 \
  --hosts host1,host2 --coord host1 \
  --dax_dev /dev/dax0.0 --dax_offset 2G --per_host_offset 20G \
  --test rd --size 1G --repetition 10 --start_delay 0 --stop_delay 2
# repeat with --test ntwr
```

### Fig 2(e) — 2 hosts, 4 CXL (Config D)

Same invocation as (d) with `--dax_dev /dev/interleave_dax` with different config for `interleave_dax` kernel module.

### Fig 2(f) — 4 hosts, 8 CXL (Config D)

```bash
./run_dax_multi_host.sh 1,2,4,8,16 \
  --hosts host1,host2,host3,host4 --coord host1 \
  --dax_dev /dev/interleave_dax --dax_offset 2G --per_host_offset 20G \
  --test rd --size 1G --repetition 10 --start_delay 0 --stop_delay 2
# repeat with --test ntwr
```

## Plotting

```sh
./plot.sh
```

Regenerates all 6 paper figures from the CSVs in `../../result/bw_mem/raw/` into
`../../result/bw_mem/plots/`. For one-off plotting, invoke `plot_bw_mem.py <prefix>`
directly from `../../result/bw_mem/raw/`.

## Individual scripts

### `run_dram_local.sh`
Run on local DRAM (NUMA / malloc).

### `run_dax_local.sh`
Run on a local or remote DAX device (single host).

### `run_dax_multi_host.sh`
Run on multiple hosts with SSH-only synchronization (start epoch + stop
epoch). Requires:

- passwordless SSH to each host
- passwordless SSH from each host back to the coordinator
- synchronized clocks (NTP) for start epochs
- repo present at `--remote_root` on each host

Options of note:

- `--coord_root` — coordinator repo path if it differs from `--remote_root`
- `--stop_delay` — how long hosts wait before exiting after all results
  are collected
- `--per_host_offset` — force non-overlapping host ranges on a shared DAX
  device

The script appends an `avg` row per thread count after all hosts report.

## Helpers

- `remote_run.sh` — invoked on each remote host by the multi-host script
- `append_result.sh` — coordinator-side callback (flock + CSV append)
- `common.sh` — shared argument parsing and CSV-schema helpers
