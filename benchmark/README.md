# benchmark/ — Artifact A3: CXL memory microbenchmarks

Reproduces the paper's bandwidth, latency, and publish/observe
round-trip measurements across DRAM, local CXL, and multi-host shared
CXL, plus the RDMA-vs-CXL network comparison (Table III). This artifact
depends on Artifact A1 (`interleave_dax` kernel
module) for the shared/interleaved memory region.

## Paper mapping

| Tool                   | Reproduces                      | Reproduction docs                                   |
|------------------------|---------------------------------|-----------------------------------------------------|
| `bw_mem/bw_mem`        | Fig 2 (a)–(f), Table I configs  | [`scripts/bw_mem/README.md`](scripts/bw_mem/README.md)                       |
| `tool_memory_latency/` | Fig 3 (a)–(c)                   | [`scripts/tool_memory_latency/README.md`](scripts/tool_memory_latency/README.md) |
| `rtt/`                 | Table II | [`scripts/rtt/README.md`](scripts/rtt/README.md)                             |
| RDMA `perftest` (`reproduce.sh rdma`) | Table III (RDMA-vs-CXL rows) | [§ RDMA-vs-CXL comparison](#rdma-vs-cxl-comparison-table-iii) |

each `result/<tool>/` holds raw data and plots.

```
result/<tool>/     # results only
├── raw/           #   raw CSVs (and notes) that feed the paper figure/table
└── plots/         #   PDF + PNG figures as they appear in the paper
scripts/<tool>/    # runners + plot wrapper + README (reproduction guide)
```

## Contents

```
benchmark/
├── Makefile                   # builds bw_mem and tool_memory_latency
├── reproduce.sh               # one-command driver (bw / lat / rtt / rdma sections)
├── bw_mem/lmbench/            # LMBENCH-derived bandwidth tool source
├── tool_memory_latency/       # pointer-chasing / flush-latency tool source
├── rtt/                       # publish/observe RTT tool 
├── util/                      # shared C++ utilities (Abseil glue, perf counters)
├── scripts/{bw_mem,rtt,tool_memory_latency}/   # runners + plot wrappers + per-tool READMEs
├── third_party/               # dependencies fetched on first build (gitignored)
├── build/                     # .o files (gitignored)
└── result/{bw_mem,tool_memory_latency,rtt,rdma}/   # raw/ + plots/ only
```

## System requirements

- Linux x86-64 with `clflushopt` / `clwb` support (Intel Ice Lake or newer)
- `gcc` / `g++` with C++20
- `cmake >= 3.16` (used to build Abseil on first `make`)
- `git` (used to fetch Abseil on first `make`)
- **Static `libnuma.a`** — install the static variant of your distro's
  libnuma package. If your distro only ships the shared library, build
  `libnuma.a` from the upstream `numactl` source.
- Internet access for the first `make` (fetches Abseil
  `20250814.1`). Override `ABSL_VERSION` / `ABSL_URL` to pin a
  different tag or mirror.

## Build

```sh
# Builds all three tools (fetches + builds Abseil on first run).
make
```

Artifacts land in `bw_mem/bw_mem`, `tool_memory_latency/tool_memory_latency`,
and `rtt/rtt`. All three are statically linked. 

Override `ABSL_PREFIX` to link against a system-installed Abseil instead:

```sh
make ABSL_PREFIX=/usr
```

Other useful targets:

- `make rtt` — build only the RTT tool (no Abseil / libnuma dep)
- `make deps` — fetch + build Abseil only
- `make abseil-clean` — remove only the fetched Abseil source tree
  under `third_party/abseil-cpp/`. Abseil will be re-fetched on the
  next `make`.
- `make clean` — remove all benchmark objects and binaries **and**
  the entire `third_party/` tree (including Abseil). The next `make`
  will re-fetch and re-build Abseil (~5 min extra). If you want to
  keep Abseil cached, clean individual build outputs manually
  (`rm -rf build bw_mem/bw_mem tool_memory_latency/tool_memory_latency rtt/rtt`).

## Reproducing data in the paper

On the testbed controller (with the hosts prepared per
`../scripts/README.md` — `cxl_remap.sh status` healthy):

```sh
# Everything: Fig 2 bandwidth (6 configs), Table II RTT, Fig 3 latency, Table III RDMA.
RUNS=3 ./reproduce.sh all

# Individual sections:
./reproduce.sh bw|lat|rtt|rdma
QUICK=1 RUNS=1 ./reproduce.sh bw
```

`reproduce.sh` handles the whole flow: preflight (host reachability,
CXL windows, builds, the host↔host SSH mesh the multi-host runner
needs, NTP), per-configuration `interleave_dax` profile switching,
running every sweep `RUNS` times (default 3), splitting results into
the per-panel CSVs under `result/*/raw/`, and regenerating the plots
(mean of the repeated runs with error bars showing the spread). The
first invocation preserves the previous raw data in
`result/*/raw/paper_baseline/`.

Fig 3 latency automation (`scripts/tool_memory_latency/reproduce_lat.py`)
finds p50/p95/p99 automatically by bisection over the tool's threshold sweeps

### RDMA-vs-CXL comparison (Table III)

`./reproduce.sh rdma` runs a `perftest` battery between the two
BlueField-3 hosts (default `g3`↔`g4`; override with `SRV=`/`CLI=`) over
both network paths — `mlx5_0` through the InfiniBand NDR switch and
`mlx5_1` direct 100G RoCE — measuring write/read latency, atomics
(fetch-add, CAS), and bandwidth. Rows land in `result/rdma/raw/rdma.csv`
and form the RDMA baseline of Table III that the paper compares CXL
publish/observe against. Requires the IB/RoCE fabric up on both hosts
(kernel `CONFIG_INFINIBAND*` / `CONFIG_MLX5_INFINIBAND`; see `../cfg/`).

### Manual reproduction

The per-tool guides linked in the table above list the exact
individual invocations behind each figure; each `scripts/<tool>/plot.sh`
regenerates that tool's figures from `result/<tool>/raw/` into
`result/<tool>/plots/`.


