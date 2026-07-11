#!/usr/bin/env python3
"""aggregate_runs.py — aggregate repeated PARSEC sync-profile runs (Table V).

Scans results/run_*/logs/run_*.log (one directory per run_all_simlarge.sh
invocation), extracts the per-benchmark ROI synchronization metrics with the
same patterns the per-run summarizer uses, and emits mean ± std across runs
to results/summaries/TABLE_V_AGG.md.

Usage: aggregate_runs.py [--results DIR] [--benchmarks a,b,c]
"""
import argparse
import glob
import os
import re
import statistics
import sys

BENCH_RE = re.compile(r"Benchmarks to run:\s*(\S+)")
ROI_RE = re.compile(r"Lockstats: ROI duration ([0-9.]+)s")
LOCK_RE = re.compile(r"lock-based sync in ROI: (yes|no) (?:\(acquisitions=([0-9]+), threads=([0-9]+)\)|\(no lock acquisitions\))")
CS_RE = re.compile(r"critical-section time sum ([0-9.]+)s, avg ([0-9.]+)us, max ([0-9.]+)us")
RATIO_RE = re.compile(r"critical-section ratio .* ([0-9.]+)%")
BARRIER_RE = re.compile(r"Barrierstats: barrier waits in ROI: (yes|no) (?:\(waits=([0-9]+), threads=([0-9]+)\)|\(count=0\))")
BARRIER_TIME_RE = re.compile(r"Barrierstats: wait time sum ([0-9.]+)s, avg ([0-9.]+)us, max ([0-9.]+)us")
BARRIER_RATIO_RE = re.compile(r"Barrierstats: wait ratio .* ([0-9.]+)%")
ATOMIC_RE = re.compile(r"Atomic ops in ROI: total=([0-9]+), threads=([0-9]+)")
ROI_TOTAL_RE = re.compile(r"Total time spent in ROI: ([0-9.]+)s")

# Table V benchmark set (paper order)
DEFAULT_BENCH = [
    "parsec.canneal", "parsec.dedup", "parsec.fluidanimate",
    "parsec.streamcluster", "splash2x.lu_ncb", "splash2.radiosity",
    "splash2.raytrace",
]


def parse_log(path):
    with open(path, encoding="utf-8", errors="ignore") as f:
        text = f.read()
    m = BENCH_RE.search(text)
    if not m:
        return None, None
    bench = m.group(1)
    roi = ROI_TOTAL_RE.search(text) or ROI_RE.search(text)
    if not roi:
        return bench, None
    roi_s = float(roi.group(1))
    rec = {"roi_s": roi_s}
    m = ATOMIC_RE.search(text)
    rec["atomic_mops_s"] = (int(m.group(1)) / roi_s / 1e6) if (m and roi_s > 0) else 0.0
    m = LOCK_RE.search(text)
    acq = int(m.group(2)) if (m and m.group(1) == "yes" and m.group(2)) else 0
    rec["lock_k_s"] = acq / roi_s / 1e3 if roi_s > 0 else 0.0
    m = CS_RE.search(text)
    rec["cs_avg_us"] = float(m.group(2)) if m else 0.0
    rec["cs_max_us"] = float(m.group(3)) if m else 0.0
    m = RATIO_RE.search(text)
    rec["cs_pct"] = float(m.group(1)) if m else 0.0
    m = BARRIER_RE.search(text)
    waits = int(m.group(2)) if (m and m.group(1) == "yes" and m.group(2)) else 0
    rec["barrier_k_s"] = waits / roi_s / 1e3 if roi_s > 0 else 0.0
    m = BARRIER_TIME_RE.search(text)
    rec["barrier_avg_us"] = float(m.group(2)) if m else 0.0
    m = BARRIER_RATIO_RE.search(text)
    rec["barrier_pct"] = float(m.group(1)) if m else 0.0
    return bench, rec


METRICS = ["atomic_mops_s", "lock_k_s", "cs_avg_us", "cs_max_us", "cs_pct",
           "barrier_k_s", "barrier_avg_us", "barrier_pct", "roi_s"]


def fmt(vals):
    if not vals:
        return "-"
    mean = statistics.mean(vals)
    if len(vals) < 2:
        return f"{mean:.2f}"
    return f"{mean:.2f}±{statistics.stdev(vals):.2f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=os.path.join(os.path.dirname(__file__), ".."))
    ap.add_argument("--benchmarks", default=",".join(DEFAULT_BENCH))
    args = ap.parse_args()
    benches = [b for b in args.benchmarks.split(",") if b]

    run_dirs = sorted(glob.glob(os.path.join(args.results, "run_*")))
    if not run_dirs:
        print("no results/run_* directories found", file=sys.stderr)
        return 1
    # data[bench][metric] = [per-run values] (latest log per bench per run dir)
    data = {b: {m: [] for m in METRICS} for b in benches}
    runs_seen = 0
    for rd in run_dirs:
        per_bench = {}
        for log in sorted(glob.glob(os.path.join(rd, "logs", "run_*.log"))):
            bench, rec = parse_log(log)
            if bench in data and rec:
                per_bench[bench] = rec        # later log wins within one run
        if per_bench:
            runs_seen += 1
        for bench, rec in per_bench.items():
            for m in METRICS:
                data[bench][m].append(rec[m])

    out = os.path.join(args.results, "summaries", "TABLE_V_AGG.md")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    hdr = ("| benchmark | atomic Mops/s | lock K/s | CS avg µs | CS max µs | CS % "
           "| barrier K/s | barrier avg µs | barrier % | ROI s | runs |")
    sep = "|" + "---|" * 11
    lines = [f"# Table V aggregate over {runs_seen} run(s)", "", hdr, sep]
    for b in benches:
        vals = data[b]
        n = len(vals["roi_s"])
        lines.append("| " + " | ".join(
            [b] + [fmt(vals[m]) for m in METRICS] + [str(n)]) + " |")
    with open(out, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {out} ({runs_seen} runs aggregated)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
