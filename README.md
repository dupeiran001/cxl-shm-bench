# Quantifying the Cost of Software-Managed Synchronization on Multi-Host CXL Memory

Artifacts for the SC26 paper of the same title. This repository contains
the source code, build scripts, measurement data, and reproduction
instructions for every experiment reported in the paper.

## Artifacts

| ID | Artifact | Path | Reproduces | Paper |
|---|---|---|---|---|
| A1 | Single-DAX interleaving kernel module                | `interleave_dax/`                | Substrate for all multi-host runs                           | §II.C |
| A2 | Global shared-memory allocator across hosts          | `global_allocator/`              | ID-based alloc + barrier + LFM / Peterson / Bakery locks    | §III, §IV |
| A3 | CXL memory microbenchmarks (+ RDMA comparison)       | `benchmark/`                     | Figure 2, Figure 3, Table II, Table III                     | §III.A–C |
| A4 | Synchronization primitives and benchmarks            | `synchronization/`               | Figure 5, Figure 6                                          | §IV |
| A5 | GAPBS graph workloads on CXL-DSM (+ SYNC_STATS profiling) | `gapbs_mh/` (+ `gapbs/`)     | Figure 7, Table VI                                          | §V |
| A6 | PARSEC/SPLASH-2 synchronization profiler             | `parsec/`                        | Table V                                                     | §IV.E |
| A7 | Concurrent hash table on CXL-DSM                     | `hashtable_mh/`                  | Figure 8, §V hash-table subsection                          | §V |

Each artifact directory has its own `README.md` with build/run/reproduce
instructions, a `result/{raw,plots}/` tree with the data and figures,
and a `scripts/` folder with the runners and plotters.


## Repository layout

```
.
├── LICENSE                 Our code: BSD 3-Clause.
├── NOTICE                  Upstream components and their licenses.
├── Makefile                Top-level orchestrator (see `make help`).
├── README.md               This file.
├── setup.sh                One-time testbed setup (remap + build + deploy)
├── scripts/                Testbed automation: prepare.sh, cxl_remap.sh, poolmap/
├── cfg/                    Kernel .config (production / debug-OFF) + build notes
├── interleave_dax/         A1 — kernel module + validate.sh + reproduce.sh
├── global_allocator/       A2 — userspace library + cross-host verifier + reproduce.sh
├── benchmark/              A3 — bw_mem + tool_memory_latency + rtt (+ rdma) + reproduce.sh
├── synchronization/        A4 — lock_bench + plot pipeline + reproduce.sh
├── gapbs/                  Unmodified upstream GAPBS (reference baseline)
├── gapbs_mh/               A5 — our multi-host GAPBS kernels + reproduce.sh
├── hashtable_mh/           A7 — concurrent hash table on CXL-DSM + reproduce.sh
└── parsec/                 A6 — modified PARSEC 3.0 + reproduce.sh
```


## One-command reproduction

One-time setup first: `./setup.sh` brings the testbed to the state all
reproduce scripts assume — it remaps the CXL fabric (`scripts/cxl_remap.sh
remap`, power-cycles g1–g4), builds every artifact on the controller,
installs SSH keys, and deploys the built tree to
`/tmp/cxl_shm_profiling` on every host (`SKIP_REMAP=1 ./setup.sh`
re-builds/re-deploys without the power-cycle).

Then: every artifact ships a `reproduce.sh` at its root that runs the
complete experiment matrix for its figures/tables — preflight checks,
repeated runs (`RUNS`, default 3), raw-data collection, and plot/table
regeneration with run-to-run variance:

```sh
interleave_dax/reproduce.sh       # A1: build/load/validate
global_allocator/reproduce.sh     # A2: unit tests + cross-host verify
benchmark/reproduce.sh all        # A3: Fig 2, Fig 3, Table II, Table III
synchronization/reproduce.sh      # A4: Fig 5, Fig 6
gapbs_mh/reproduce.sh all         # A5: Fig 7 (incl. 4-host configs)
gapbs_mh/profile_sync.sh          # A5: Table VI (SYNC_STATS profiling)
parsec/reproduce.sh               # A6: Table V
hashtable_mh/reproduce.sh all     # A7: Fig 8 (concurrent hash table)
```

These scripts drive the multi-host testbed from a controller machine
(SSH). `scripts/` holds the supporting testbed automation: per-boot host
preparation (`prepare.sh`) and shared-CXL fabric mapping/recovery
(`cxl_remap.sh`). See
`scripts/README.md`. Prior raw data is preserved under
`result/*/raw/paper_baseline/` the first time a reproduction overwrites
it.


## Getting started

From the repository root:

```sh
./setup.sh       # one-time testbed setup: remap + build + deploy to hosts
make help        # list top-level targets
make all         # build A2, A3, A4, A5, A7 (nothing that needs root)
make kernel      # build A1 (requires root + kernel headers)
make parsec      # fetch + build A6 (downloads ~500 MB of inputs)
```

After building, follow each artifact's `README.md` to run the
experiments and regenerate the figures from `result/raw/` into
`result/plots/`.

## System requirements

- **Hardware.** The paper's measurements ran on an Intel Xeon 6787P
  platform with up to four hosts sharing pooled CXL memory through an
  H3 Falcon C5022 memory server built on the XConn XC50256 CXL switch
  (see paper §II.C).
  A1 / A2 / A4 / A5 / A7 need a similar multi-host CXL topology for the
  cross-host results; single-host experiments need any Linux x86-64
  system with `clflushopt` and `clwb` support (Intel Ice Lake or
  newer).
- **Kernel.** All measurements use a **production (debug-OFF)** build of
  mainline Linux at commit
  [`0138af2472df`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=0138af2472dfdef0d56fc4697416eaa0ff2589bd)
  (`v7.0-rc5`+79 ≈ 7.0-rc6, from `torvalds/linux`), built with GCC 15.2.1 —
  no local patches. The exact `.config` and build steps are in
  [`cfg/`](cfg/README.md); it enables `CONFIG_DEV_DAX*`,
  `CONFIG_CXL_{BUS,MEM}`, `CONFIG_EFI_SOFT_RESERVE`, and
  `CONFIG_INFINIBAND*`/`CONFIG_MLX5_INFINIBAND` (for the RDMA comparison),
  with kernel debug and lock validation **off** (a debug kernel inflates the
  synchronization measurements the paper reports). A1 (the out-of-tree
  kernel module) builds against a source tree at this same commit:
  `./setup.sh` (or `scripts/fetch_kernel.sh`) shallow-fetches the pinned
  commit (~300 MB, one-time), applies the shipped `cfg/` config, and runs
  `make modules_prepare` automatically when no tree is present at
  `linux-pxe/linux` (override with `KDIR=<path>` to reuse an existing
  tree).
- **Toolchain.** `gcc 15` / `g++` with C++20 for A1–A5, `cmake ≥ 3.16`
  for A3's Abseil build, `bash ≥ 4` for the runner scripts, Python ≥ 3.9
  with `matplotlib`, `pandas`, `numpy` for plot regeneration.
- **Distro packages.** `libnuma-dev` (or the distro equivalent that
  provides static `libnuma.a`) for A3; `libcriterion` for A2's unit
  tests; `git` for A3's on-demand Abseil fetch.
- **Internet.** First `make benchmark` will fetch Abseil (pinned tag
  20250814.1). A6 downloads PARSEC's `sim` inputs (~490 MB,
  SHA-256-verified) at first setup from their archival Zenodo mirror
  ([doi:10.5281/zenodo.21314344](https://doi.org/10.5281/zenodo.21314344));
  pre-place the tarball under `parsec/input-archive/` to run offline.
  `./setup.sh` shallow-fetches the pinned kernel source (~300 MB) for the
  A1 module build when no `KDIR` tree exists. Mirror URLs can be
  overridden via env vars; see `benchmark/Makefile`, `parsec/setup.sh`,
  and `scripts/fetch_kernel.sh`.


## CXL module configuration — full (8-module) vs. current testbed (4-module)

The paper's platform (§II) populates the H3 Falcon CXL memory server with **8 Micron
CZ120 128&nbsp;GB CXL Type-3 modules**. Reproduction touches this at two scales:

- **Cross-host shared-memory workloads** — locks (Figure 5–6), GAPBS (Figure 7), the
  concurrent hash table (Figure 8), and Tables II/III/VI — run on a **512&nbsp;GB shared
  window per host = 4 interleaved modules**, mapped identically to all four hosts by the
  switch pool-map `scripts/poolmap/Poolmap_sc_ae.sh` (pushed and applied by
  `scripts/cxl_remap.sh remap`).
- **The bandwidth characterization** (Figure 2; Config D in Table I) sweeps **1–8
  interleaved devices**; its highest-device-count points require all 8 modules.

### Running the full 8-module benchmark

Populate the H3 Falcon with **8 free Micron CZ120 modules** on the switch downstream ports and
extend the pool map to assign all 8 to the endpoint ports — the bundled
`scripts/poolmap/Poolmap_sc_ae.sh` maps ports `5 21 4 20` (4×128&nbsp;GB → 512&nbsp;GB
per host); add the remaining four ports and raise each host's `sethdmsize` accordingly.
With 8 modules the full 1–8-device sweep of Figure 2(d)–(f) reproduces, on top of all
the 4-host shared workloads.

> ### Current testbed limitation: only 4 modules are available
>
> The shared H3 does **not currently have 8 free Micron modules.** Only **4 Micron
> 128&nbsp;GB modules** are available to the g-hosts (downstream ports `5 21 4 20`). **This
> artifact is therefore exercised at the 4-module configuration** — the 512&nbsp;GB/host
> shared window that every cross-host result uses (Figures 5–8, Tables II/III/VI), plus
> the bandwidth sweep up to 4 interleaved devices. Reproducing the higher-device-count
> points of Figure 2(d)–(f) requires repopulating the H3 with 8 free Micron modules and
> extending `Poolmap_sc_ae.sh` as above. All other results are unaffected by the module
> count.

## License and attribution

The code we authored is licensed under **BSD 3-Clause**; see `LICENSE`.
The repository also redistributes or fetches third-party components
(GAPBS, PARSEC 3.0, LMBench, Abseil) under their respective upstream
licenses. See `NOTICE` for the full list.
