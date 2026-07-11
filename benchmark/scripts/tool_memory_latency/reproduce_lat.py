#!/usr/bin/env python3
"""reproduce_lat.py — automated Fig 3 latency reproduction.

Replaces the manual workflow (hand-tuned --lat_low/high/step windows + reading
"threshold N cycles: X% <= threshold" lines by eye) with an automatic
percentile finder: the fraction of accesses <= threshold is monotonic in the
threshold, so p50/p95/p99 are found by bisection on single-threshold probes.

Each probe = one measurement run of tool_memory_latency on the target host
(over SSH), with --lat_low == --lat_high == candidate threshold. The tool
prints, per threshold:  threshold <N> cycles: <PCT>% <= threshold, avg <A> cycles

Outputs the nine raw CSVs the existing plotter expects
(<config>_<test>.csv in ../../result/tool_memory_latency/raw):
  rd_miss files:                 measurement,value        (avg,p50,p95,p99)
  store_flush/flush_clean files: measurement,value,method (x clwb,clflushopt)

Usage:
  reproduce_lat.py --host g1.dc --local-dax /dev/dax1.0 \
                   --remote-dax /dev/interleave_dax [--quick] [--out DIR]
"""
import argparse
import re
import statistics
import subprocess
import sys
from pathlib import Path

LINE_RE = re.compile(
    r"threshold\s+(\d+)\s+cycles:\s+([\d.]+)%\s+<=\s+threshold,\s+avg\s+([\d.]+)\s+cycles"
)

REMOTE_BIN = "/tmp/cxl_shm_profiling/benchmark/tool_memory_latency/tool_memory_latency"

# (csv test name, binary --test value, size args)
TESTS = [
    ("rd_miss", "rd", ["--sz=1G", "--total_sz=8G"]),
    ("store_flush", "store_flush", ["--sz=32M", "--total_sz=256M"]),
    ("flush_clean", "flush_clean", ["--sz=32M", "--total_sz=256M"]),
]
PCTS = [50, 95, 99]


def ssh(host, args, timeout=600):
    cmd = ["ssh", "-4", "-o", "BatchMode=yes", f"root@{host}"] + [" ".join(args)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.stdout + r.stderr


class Prober:
    """One measurement configuration; caches probe results by threshold."""

    def __init__(self, host, base_args, quick=False):
        self.host = host
        self.base = base_args
        self.cache = {}          # threshold -> pct
        self.avgs = []
        self.quick = quick

    def probe(self, threshold):
        threshold = int(threshold)
        if threshold in self.cache:
            return self.cache[threshold]
        args = [REMOTE_BIN] + self.base + [
            f"--lat_low={threshold}", f"--lat_high={threshold}", "--lat_step=1"]
        out = ssh(self.host, args)
        m = LINE_RE.search(out)
        if not m:
            raise RuntimeError(f"unparsable probe output for {args}:\n{out[-800:]}")
        pct, avg = float(m.group(2)), float(m.group(3))
        self.cache[threshold] = pct
        self.avgs.append(avg)
        return pct

    def percentile(self, target, lo, hi):
        """Smallest threshold with pct >= target, via bisection in [lo, hi]."""
        while self.probe(hi) < target:
            lo, hi = hi, hi * 2
            if hi > 200_000:
                raise RuntimeError(f"p{target}: no crossing below 200k cycles")
        tol = 0.05 if self.quick else 0.01           # relative threshold tolerance
        while hi - lo > max(2, lo * tol):
            mid = (lo + hi) // 2
            if self.probe(mid) < target:
                lo = mid
            else:
                hi = mid
        return hi

    def measure(self):
        """Returns dict avg/p50/p95/p99 (cycles)."""
        res = {}
        lo, hi = 20, 2000
        for t in PCTS:
            p = self.percentile(t, lo, hi)
            res[f"p{t}"] = p
            lo, hi = p, max(p * 2, p + 50)           # monotonic: reuse as lower bound
        res["avg"] = round(statistics.mean(self.avgs), 2)
        return res


def config_args(config, test_flags, local_dax, remote_dax):
    if config == "dram":
        return ["-m", "0", "-n", "0"] + test_flags
    if config == "local_cxl":
        return [f"--dax_dev={local_dax}"] + test_flags
    if config == "remote_cxl":
        return [f"--dax_dev={remote_dax}"] + test_flags
    raise ValueError(config)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="g1.dc")
    ap.add_argument("--local-dax", default="/dev/dax1.0")
    ap.add_argument("--remote-dax", default="/dev/interleave_dax")
    ap.add_argument("--out", default=str(Path(__file__).resolve().parent.parent.parent / "result" / "tool_memory_latency" / "raw"))
    ap.add_argument("--quick", action="store_true")
    args = ap.parse_args()
    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)

    for csv_test, bin_test, size_flags in TESTS:
        for config in ("dram", "local_cxl", "remote_cxl"):
            path = outdir / f"{config}_{csv_test}.csv"
            rows = []
            if csv_test == "rd_miss":
                base = config_args(config, [f"--test={bin_test}"] + size_flags,
                                   args.local_dax, args.remote_dax)
                print(f"[lat] {config}/{csv_test}", flush=True)
                r = Prober(args.host, base, args.quick).measure()
                rows.append(("measurement,value",
                             [f"avg,{r['avg']}", f"p50,{r['p50']}",
                              f"p95,{r['p95']}", f"p99,{r['p99']}"]))
            else:
                lines = []
                for method in ("clwb", "clflushopt"):
                    base = config_args(
                        config,
                        [f"--test={bin_test}", f"--flush={method}"] + size_flags,
                        args.local_dax, args.remote_dax)
                    print(f"[lat] {config}/{csv_test}/{method}", flush=True)
                    r = Prober(args.host, base, args.quick).measure()
                    lines += [f"avg,{r['avg']},{method}", f"p50,{r['p50']},{method}",
                              f"p95,{r['p95']},{method}", f"p99,{r['p99']},{method}"]
                rows.append(("measurement,value,method", lines))
            header, lines = rows[0]
            path.write_text(header + "\n" + "\n".join(lines) + "\n")
            print(f"[lat] wrote {path}", flush=True)
    print("[lat] all nine CSVs written", flush=True)


if __name__ == "__main__":
    sys.exit(main())
