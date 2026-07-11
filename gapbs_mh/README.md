# gapbs_mh — Artifact A5: multi-host GAPBS on CXL-DSM

Multi-host adaptation of the GAPBS graph kernels, running on a shared
CXL memory region with explicit software-managed visibility (no
cross-host cache coherence). Each host runs a separate binary instance;
coordination uses `shm_barrier_t` from Artifact A2 (`global_allocator`)
and per-kernel synchronization primitives built on top.

## Paper mapping

Reproduces **Figure 7 (a)–(d)** — runtime scaling of four kernels
across local DRAM, single-host remote CXL, and two-host shared CXL:

| Panel | Kernel | Binary | WSS |
|---|---|---|---|
| Fig 7(a) | BFS (direction-optimizing) | `bfs_mh` | ≈ 51.5 GiB (g=27, k=50) |
| Fig 7(b) | PR (Gauss–Seidel, owner-local) | `pr_mh` | ≈ 13.0 GiB (g=25, k=50) |
| Fig 7(c) | PR-SPMV (Jacobi, owner-local) | `pr_spmv_mh` | ≈ 13.0 GiB (g=25, k=50) |
| Fig 7(d) | TC (read-dominated) | `tc_mh` | ≈ 0.4 GiB (g=20, k=50) |

**Table VI** (workload-fit summary): its measured columns (publish
lines/s and sync-time share) come from separate SYNC_STATS-instrumented
runs driven by `profile_sync.sh` — see
[Sync profiling](#sync-profiling-table-vi) below.

## Contents

```
gapbs_mh/
├── Makefile
├── README.md
├── reproduce.sh         one-command driver → Figure 7
├── profile_sync.sh      SYNC_STATS-instrumented runs → Table VI
├── src/
│   ├── bfs_mh.cc, pr_mh.cc, pr_spmv_mh.cc, tc_mh.cc
│   ├── shm_graph.h     shared CSR layout + init helpers
│   ├── shm_support.h   per-host partition + barrier utilities
│   └── cxl_util.h      CxlLoad / CxlStore / CxlHashedCAS
├── scripts/  plot_gapbs.py, sync_profile.py + README (figure + Table VI helpers)
└── result/
    ├── raw/    gapbs.csv + per-kernel raw run logs
    └── plots/  Figure 7 (a)-(d) — pdf + png
```

## Build

```sh
# 1. Build the global_allocator library (Artifact A2)
make -C ../global_allocator lib

# 2. Build the four gapbs_mh kernels
make
```

Produces `bfs_mh`, `pr_mh`, `pr_spmv_mh`, `tc_mh` at the top level of
this directory.

Requires:
- `g++` with C++11 + OpenMP + `-mclflushopt`
- `../global_allocator/build/libglobal_allocator.a`
- `../gapbs/src/*.h` (upstream GAPBS headers — single-host reference
  that we include for graph-generation utilities)

## Run

The one-command driver is `./reproduce.sh` (preflight → sweeps → figures; see
the top-level `README.md`). To run the kernels by hand: all four use input
graphs from the GAPBS Kronecker generator (`-g` = 2^g vertices, `-k` = average
degree). Use `--dev=/dev/interleave_dax` for the multi-host shared config,
`/dev/dax0.0` for single-host remote CXL, and drop the flag for local DRAM.
Requires Artifact A1 (`interleave_dax`) loaded on every host so
`/dev/interleave_dax` is present.

### BFS — Fig 7(a) (WSS ≈ 51.5 GiB)

```sh
# on each host, with $HOST_ID ∈ {0,1}; repeat at OMP_NUM_THREADS ∈ {4,8,16,32}
OMP_NUM_THREADS=$T ./bfs_mh -g 27 -k 50 \
    --host_id=$HOST_ID --hosts=2 \
    --dev=/dev/interleave_dax --shm-mb=64000 -n 10 -l
```

### PR — Fig 7(b) (WSS ≈ 13.0 GiB)

```sh
OMP_NUM_THREADS=$T ./pr_mh -g 25 -k 50 \
    --host_id=$HOST_ID --hosts=2 \
    --dev=/dev/interleave_dax --shm-mb=64000 -n 1 --sync-k 1 -l
```

### PR-SPMV — Fig 7(c) (WSS ≈ 13.0 GiB)

```sh
OMP_NUM_THREADS=$T ./pr_spmv_mh -g 25 -k 50 \
    --host_id=$HOST_ID --hosts=2 \
    --dev=/dev/interleave_dax --shm-mb=64000 -n 3 -l
```

### TC — Fig 7(d) (WSS ≈ 0.4 GiB)

```sh
OMP_NUM_THREADS=$T ./tc_mh -g 20 -k 50 \
    --host_id=$HOST_ID --hosts=2 \
    --dev=/dev/interleave_dax --shm-mb=16384 -n 3 -l
```

Thread sweep: `T ∈ {4, 8, 16, 32}`. For the two-host config, start both hosts at
the same time; `--host_id=0` initializes the shared CSR while host 1 attaches.
Each kernel × thread-count × config run emits one per-trial runtime line;
averaged across trials it becomes one row in `result/raw/gapbs.csv` (schema:
`benchmark,config,total_threads,threads_per_host,g,k,trials,avg_s,stages,cmd_host0,cmd_host1`).
The full per-kernel stdout from the paper run is archived under
`result/raw/{bfs,pr,pr_spmv,tc}_runs.md`.

### Common CLI flags

| Flag | Meaning |
|---|---|
| `-g <N>` | graph scale (2^N vertices, Kronecker generator) |
| `-k <N>` | average degree |
| `-n <N>` | number of trials (we average across) |
| `--host_id=<N>` | this host's index (0 = initializer) |
| `--hosts=<N>` | total participating hosts |
| `--dev=<path>` | shared DAX device (`/dev/interleave_dax` for multi-host) |
| `--shm-mb=<M>` | shared memory mmap size in MB |
| `-l` | enable per-level logging |
| `--sync-k <N>` | (PR only) Gauss–Seidel sync cadence |

### Regenerating the figures

```sh
python3 scripts/plot_gapbs.py     # reads result/raw/gapbs.csv → result/plots/
```

Writes Figure 7 (a)–(d) as `fig_gapbs_<kernel>_wss_<WSS>.{pdf,png}`; `--bench
<KERNEL>` regenerates one panel, `--csv`/`--out_dir` override the defaults.

## Sync profiling (Table VI)

`profile_sync.sh` produces the quantitative columns of **Table VI** (the
workload-fit summary). The `SYNC_STATS` counters perturb timing, so these runs
are kept separate from the `reproduce.sh` timing runs:

```sh
gapbs_mh/profile_sync.sh          # QUICK=1 for a fast smoke pass
```

It rebuilds `gapbs_mh` + `global_allocator` with `SYNC_STATS=1` on all four
hosts, runs one instrumented trial per kernel for the shared 2-host and 4-host
configurations, collects the per-host counter logs, and runs
`scripts/sync_profile.py` to emit `result/raw/TABLE_VI_QUANT.md` (per-kernel
publish lines/s and sync-time share), then restores the clean
(un-instrumented) builds.

## Data placement and sync mechanism

Each kernel uses the shared CXL region for the graph CSR and one or
more per-vertex arrays; host-local state (frontiers, per-thread
counters) lives in DRAM. Cross-host updates use one of three patterns:

- **Hashed CAS via LFM locks** (BFS top-down) — 65536-entry lock pool
  indexed by a hash of the target vertex; avoids per-vertex lock
  overhead while serialising conflicting writes.
- **Owner-local updates + bulk flush** (PR, PR-SPMV) — each host owns a
  vertex range; inter-host visibility is handled by a per-iteration
  barrier + `clwb`-and-fence on the published range.
- **Read-only** (TC) — no inter-host synchronization during the main
  phase; a final reduction via software atomics.

Details and numbers are in the paper §V and the per-kernel source
comments.

## Synchronization primitives (from Artifact A2)

| Primitive | Header | Usage |
|---|---|---|
| `shm_mutex_t` (LFM backend) | `global_allocator/shm_mutex.h` | hashed CAS in BFS |
| `shm_barrier_t` | `global_allocator/barrier.h` | per-level / per-trial barriers |
| `CxlLoad` / `CxlStore` | `src/cxl_util.h` | flush-and-fence wrappers for non-coherent reads/writes |
| `CxlHashedCAS` | `src/cxl_util.h` | hashed CAS helper |

## Clean

```sh
make clean
```
