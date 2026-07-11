# parsec — Artifact A6

Reproduces **Table V** of the paper (synchronization characteristics of
PARSEC and SPLASH-2/2x workloads). All measurements are taken on a
single coherent host — the point is to characterize each workload's
intrinsic synchronization intensity (atomic-op rate, lock-acquisition
rate, critical-section durations, barrier waits) **independently of
CXL**, then compare those intrinsic costs against the ~1 µs visibility
and ~3–5 µs lock-acquisition costs measured on CXL-DSM in Sections
III–IV. No CXL hardware is needed to run this artifact.

## Paper mapping

- **Table V** rows: `parsec.canneal`, `parsec.dedup`, `parsec.fluidanimate`,
  `parsec.streamcluster`, `splash2x.lu_ncb`, `splash2.radiosity`,
  `splash2.raytrace`.
- `run_all_simlarge.sh` runs **all** applicable benchmarks (kernels,
  apps, SPLASH-2, SPLASH-2x) with the `simlarge` input; Table V reports
  the seven rows above and discards the rest. The reference output of
  our paper run is in `results/`.

## Layout

```
parsec/
├── bin/              PARSEC management tools (parsecmgmt, bldconfadd, ...)
├── config/           global build/run configs
├── pkgs/             benchmark sources, grouped as apps/ kernels/ libs/ tools/ netapps/
│   └── libs/hooks/   ← our modified instrumentation library (atomic / lock / barrier)
├── ext/              SPLASH-2 and SPLASH-2x sources
├── man/              upstream man pages
├── setup.sh          fetches the input tarball (Zenodo mirror) and builds with gcc-hooks
├── run_all_simlarge.sh  drives the full benchmark sweep + parses logs
└── results/          our raw paper-run logs + computed summary tables
    ├── logs/              41 raw run_*.log files (the actual run output)
    └── summaries/
        ├── ATOMIC_COUNTS.md       atomic-ops-per-ROI, all 40 benchmarks
        └── ATOMIC_LOCK_COUNTS.md  full sync stats (atomic, lock, barrier), all 40 benchmarks
```

## Licensing and redistribution

This directory **includes modified PARSEC 3.0 source** (Princeton
University, BSD-style license — see `LICENSE` at this directory's
root and per-package `LICENSE` files under `pkgs/*/*/src/`). Our
modifications are localized to `pkgs/libs/hooks/` (instrumentation
that records per-ROI atomic, lock, and barrier statistics) plus minor
build-config fixes noted in `CHANGELOG`.

Input datasets are **not** redistributed in this repository. `setup.sh`
downloads the simulation input dataset (`parsec-3.0-input-sim.tar.gz`,
~490 MB) at first use from its archival Zenodo mirror
([doi:10.5281/zenodo.21314344](https://doi.org/10.5281/zenodo.21314344);
the original Princeton URL is defunct) and verifies its SHA-256 before
extraction. To run offline, pre-place the tarball under
`input-archive/`.

## Setup

```sh
./setup.sh
```

This script will do:

1. Downloads `parsec-3.0-input-sim.tar.gz` (~490 MB, SHA-256-verified)
   from the archival Zenodo mirror
   ([doi:10.5281/zenodo.21314344](https://doi.org/10.5281/zenodo.21314344)),
   unless the file is already present (or pre-placed under
   `input-archive/` for offline runs).
2. Extracts it, which populates `pkgs/*/inputs/` and
   `ext/*/apps/*/inputs/` with the simlarge + smaller input sets.
3. Builds the suite with the `gcc-hooks` configuration
   (`./bin/parsecmgmt -a build -c gcc-hooks -p apps -p kernels -p splash2 -p splash2x`).

Optional env overrides for `setup.sh` (all default to the above
behavior):

- `PARSEC_INPUT=native` — also fetch the 2.3 GB native input tarball.
- `PARSEC_INPUT=both` — fetch both sim and native.
- `PARSEC_BUILD=no` — fetch + extract only, skip the build.
- `PARSEC_SIM_URL=<url>` / `PARSEC_NATIVE_URL=<url>` — override the
  download URL. `PARSEC_SIM_URL` defaults to the archival Zenodo mirror
  (doi:10.5281/zenodo.21314344); downloads are SHA-256-verified. The
  optional `native` inputs have no live default (the historical
  Princeton URL is defunct).

## Run

After `setup.sh` completes:

```sh
./run_all_simlarge.sh
```

Runs all kernels + apps + SPLASH-2 + SPLASH-2x with `simlarge` input
and 16 threads (override via `INPUT`, `THREADS`, `CONFIG`,
`SPLASH2_INPUT` env vars). Output lands in
`results/run_<TIMESTAMP>/`:

- `logs/run_*.log` — raw per-benchmark output
- `summaries/ATOMIC_LOCK_COUNTS.md` — the big table (atomic ops, lock
  acquisitions, critical-section durations, barrier stats, ROI time)
- `summaries/ATOMIC_COUNTS.md` — atomic-ops-only view
- `summaries/SANITY.md` — listing of any benchmarks whose output
  looked suspicious (missing ROI, non-zero error status)

Cross-check your `ATOMIC_LOCK_COUNTS.md` with `results/summaries/ATOMIC_LOCK_COUNTS.md`
(our paper run) for the seven Table V benchmarks listed above; minor
differences are expected due to hardware / kernel variation.

## Notes and gotchas

- `parsec.streamcluster` does not ship a `simlarge` input archive; it
  generates data internally, so you will see a harmless "No archive for
  input 'simlarge' available" message.
- Some benchmarks (e.g. `parsec.vips`, most of SPLASH-2 `simsmall`)
  don't emit hooks without explicit `__parsec_roi_*` calls — those
  rows will show zero atomics/locks and are excluded from Table V.
- `parsec.raytrace` in this tree is patched to exit cleanly after the
  ROI; upstream it can hang.

## Patch summary (vs upstream PARSEC 3.0)

Relative to the 2011 upstream release:

- `pkgs/libs/hooks/src/` instrumented to count atomic ops, lock
  acquisitions + critical-section durations, and barrier waits during
  each ROI; results are logged to the standard hooks output channel.
- Build-system fixes for modern toolchains (Arch Linux, x86-64): the
  main ones are documented in `CHANGELOG` (updated OpenSSL and `uctcpip`
  patches; global `CC` / `CFLAGS` are preserved instead of
  overwritten).

For the full unmodified upstream, see the PARSEC project page and the
per-package `LICENSE` files.
