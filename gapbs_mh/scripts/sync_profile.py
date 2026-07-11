#!/usr/bin/env python3
"""sync_profile.py — Table VI quantification (SYNC_STATS).

Parses instrumented gapbs_mh run logs (binaries built with SYNC_STATS=1) and
produces per-kernel synchronization rates plus a first-order estimate of the
synchronization time share, using the paper's measured cost constants.

Input: log files named <kernel>_<config>_h<hostid>.log, each containing the
kernel's stdout: "Average Time: <s>" (and/or "Trial Time"), plus one
"SYNC_STATS tag=... publish_lines=... observe_lines=... lock_acquires=...
lock_retries=... barrier_waits=... atomic_rmw=..." line per host process.

Counters are summed across hosts; rates divide by the measured kernel time.
Note: lock/barrier/atomic implementations internally perform cacheline
publish/observe operations, so line counts include primitive-internal
traffic — the "primitive share" estimate below deliberately uses only the
event-level costs to avoid double counting.

Usage: sync_profile.py --logs <dir> [--out ../result/raw/TABLE_VI_QUANT.md]
"""
import argparse
import glob
import os
import re
import sys
from collections import defaultdict

# Measured cost constants from the paper (§III-D, §IV):
COST = {
    "lock_s": 3.5e-6,      # lock acquisition 3-5 us -> 3.5 us midpoint
    "atomic_s": 3.0e-6,    # software atomic ~3 us
    "barrier_s": 1.0e-3,   # cross-host barrier: ms-scale
}

STATS_RE = re.compile(
    r"SYNC_STATS tag=(\S+) publish_lines=(\d+) observe_lines=(\d+) "
    r"lock_acquires=(\d+) lock_retries=(\d+) barrier_waits=(\d+) atomic_rmw=(\d+)")
AVG_RE = re.compile(r"Average Time:\s+([0-9.]+)")
FNAME_RE = re.compile(r"([a-z_]+?)_(shared_[a-z0-9]+)_h(\d+)\.log$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", required=True)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    out_path = args.out or os.path.join(
        os.path.dirname(__file__), "..", "result", "raw", "TABLE_VI_QUANT.md")

    # data[(kernel, config)] = {"time": avg_s from host0, counters summed}
    data = defaultdict(lambda: defaultdict(float))
    for path in sorted(glob.glob(os.path.join(args.logs, "*.log"))):
        m = FNAME_RE.search(os.path.basename(path))
        if not m:
            continue
        kernel, config, hostid = m.group(1), m.group(2), int(m.group(3))
        text = open(path, encoding="utf-8", errors="ignore").read()
        s = STATS_RE.search(text)
        if not s:
            print(f"WARN: no SYNC_STATS in {path}", file=sys.stderr)
            continue
        key = (kernel, config)
        for i, f in enumerate(["publish_lines", "observe_lines", "lock_acquires",
                               "lock_retries", "barrier_waits", "atomic_rmw"]):
            data[key][f] += int(s.group(i + 2))
        if hostid == 0:
            a = AVG_RE.search(text)
            if a:
                data[key]["time"] = float(a.group(1))
        data[key]["hosts"] = max(data[key]["hosts"], hostid + 1)

    lines = [
        "# Table VI quantification — measured synchronization rates",
        "",
        "First-order sync share = (locks x {:.1f}us + atomics x {:.1f}us + "
        "barriers x {:.0f}us) / runtime.".format(
            COST["lock_s"] * 1e6, COST["atomic_s"] * 1e6, COST["barrier_s"] * 1e6),
        "",
        "| kernel | config | hosts | time s | lock/s | atomic/s | barrier/s "
        "| publish lines/s | observe lines/s | est. sync share |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    for (kernel, config), v in sorted(data.items()):
        t = v.get("time", 0.0)
        if t <= 0:
            print(f"WARN: no runtime for {kernel}/{config}", file=sys.stderr)
            continue
        rate = lambda f: v[f] / t
        share = (v["lock_acquires"] * COST["lock_s"]
                 + v["atomic_rmw"] * COST["atomic_s"]
                 + v["barrier_waits"] * COST["barrier_s"]) / (
                     t * max(v["hosts"], 1))
        lines.append(
            "| {} | {} | {:.0f} | {:.3f} | {:,.0f} | {:,.0f} | {:,.1f} "
            "| {:,.0f} | {:,.0f} | {:.1%} |".format(
                kernel, config, v["hosts"], t, rate("lock_acquires"),
                rate("atomic_rmw"), rate("barrier_waits"),
                rate("publish_lines"), rate("observe_lines"), share))
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"wrote {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
