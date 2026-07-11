# Sanity issues

- parsec.vips failed to run on the paper hardware: the
  gcc-hooks-built binary depends on `libxml2.so.16` at runtime, which
  was not available on that host. The row in
  `ATOMIC_LOCK_COUNTS.md` / `ATOMIC_COUNTS.md` is therefore marked
  `error` with all-zero counters (log:
  `run_2026-02-03_02:56:12.log`).
  parsec.vips is **not** one of the seven benchmarks used in Table V;
  this failure does not affect the paper's results. A reproducer
  should either install a compatible libxml2 or skip vips.

- No other benchmark in this run had a non-zero exit code, a missing
  region-of-interest marker, or any other parsed anomaly.
