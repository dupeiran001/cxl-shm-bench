#!/usr/bin/env python3
import argparse
import csv
import os
import sys
from collections import defaultdict


TESTS = ["rd_miss", "store_flush", "flush_clean"]
METHOD_ORDER = ["clwb", "clflushopt", ""]
CONFIG_ORDER = ["dram", "local_cxl", "localcxl", "remote_cxl"]


def read_latency_csv(path):
    results = defaultdict(dict)
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            meas = (row.get("measurement") or "").strip()
            val = row.get("value")
            if meas == "" or val is None:
                continue
            try:
                val = float(val)
            except Exception:
                continue
            method = (row.get("method") or "").strip()
            results[method][meas] = val
    return results


def detect_test(name):
    for test in sorted(TESTS, key=len, reverse=True):
        suffix = "_" + test
        if name.endswith(suffix):
            return test, name[: -len(suffix)]
    return None, None


def order_methods(methods):
    ordered = []
    for m in METHOD_ORDER:
        if m in methods:
            ordered.append(m)
    for m in sorted(methods):
        if m not in ordered:
            ordered.append(m)
    return ordered


def order_configs(configs):
    ordered = []
    for cfg in CONFIG_ORDER:
        if cfg in configs:
            ordered.append(cfg)
    for cfg in sorted(configs):
        if cfg not in ordered:
            ordered.append(cfg)
    return ordered


def choose_step(max_val):
    if max_val <= 0:
        return 10
    candidates = [5, 10, 20, 50, 100, 200, 500]
    best_step = candidates[-1]
    best_dist = None
    for step in candidates:
        ticks = max_val / step
        if 3 <= ticks <= 8:
            return step
        dist = abs(ticks - 5)
        if best_dist is None or dist < best_dist:
            best_dist = dist
            best_step = step
    return best_step


def plot_test(test, data, out_path, show_legend):
    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import MultipleLocator
    except Exception as exc:
        print(f"matplotlib required: {exc}", file=sys.stderr)
        return 1
    plt.rcParams.update(
        {
            "font.size": 14,
            "axes.labelsize": 15,
            "xtick.labelsize": 13,
            "ytick.labelsize": 13,
            "legend.fontsize": 13,
        }
    )

    configs = order_configs(data.keys())
    methods = set()
    for cfg in configs:
        methods.update(data[cfg].keys())
    methods = order_methods(methods)
    if methods == [""]:
        methods = [""]

    method_colors = {
        "clwb": "#FFFFFF",
        "clflushopt": "#BFD4EA",
        "": "#FFFFFF",
    }
    method_hatches = {
        "clwb": "",
        "clflushopt": "\\\\\\",
        "": "",
    }

    fig_width = 3
    x_step = 0.15
    if test == "rd_miss":
        fig_width = 2
        x_step = 0.12
    fig, ax = plt.subplots(figsize=(fig_width, 3.6))
    group_width = 0.1
    if test == "rd_miss":
        group_width = 0.05
    bar_width = group_width if len(methods) == 1 else group_width / len(methods)
    offsets = []
    if len(methods) == 1:
        offsets = [0.0]
    else:
        start = -group_width / 2 + bar_width / 2
        offsets = [start + i * bar_width for i in range(len(methods))]

    max_val = 0.0
    for cfg_idx, cfg in enumerate(configs):
        x_base = cfg_idx * x_step
        edge_color = "#2E3440"
        for m_idx, method in enumerate(methods):
            base_color = method_colors.get(method, "#D0D0D0")
            hatch = method_hatches.get(method, "")
            series = data[cfg].get(method, {})
            avg = series.get("avg")
            p50 = series.get("p50")
            p95 = series.get("p95")
            p99 = series.get("p99")
            if avg is None:
                continue
            x = x_base + offsets[m_idx]
            ax.bar(
                x,
                avg,
                width=bar_width * 0.9,
                color=base_color,
                hatch=hatch,
                edgecolor=edge_color,
                linewidth=1.2,
                zorder=2,
            )
            if p50 is not None and p99 is not None:
                ax.vlines(x, p50, p99, color="#2E3440", linewidth=1.6, zorder=3)
                tick_w = bar_width * 0.6
                ax.hlines(
                    p50,
                    x - tick_w / 2,
                    x + tick_w / 2,
                    color="#2E3440",
                    linewidth=1.6,
                    zorder=3,
                )
                ax.hlines(
                    p99,
                    x - tick_w / 2,
                    x + tick_w / 2,
                    color="#2E3440",
                    linewidth=1.6,
                    zorder=3,
                )
            if p95 is not None:
                mid_w = bar_width * 0.5
                ax.hlines(
                    p95,
                    x - mid_w / 2,
                    x + mid_w / 2,
                    color="#5F6673",
                    linewidth=1.6,
                    linestyle="-",
                    zorder=2.6,
                )
            for v in (avg, p50, p95, p99):
                if v is not None:
                    max_val = max(max_val, v)

    ax.set_ylabel("Latency (CPU cycles)")
    ax.set_xlabel("")
    ax.set_xticks([i * x_step for i in range(len(configs))])
    label_map = {
        "dram": "DRAM",
        "local_cxl": "local CXL",
        "localcxl": "local CXL",
        "remote_cxl": "remote CXL",
    }
    ax.set_xticklabels(
        [label_map.get(c, c.replace("_", " ")) for c in configs],
        rotation=25,
        ha="right",
    )
    ax.tick_params(axis="both", width=1.2)
    ax.set_ylim(bottom=0)
    step = choose_step(max_val)
    ax.yaxis.set_major_locator(MultipleLocator(step))
    ax.grid(True, axis="y", linestyle=(0, (4, 6)), alpha=0.5)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["bottom"].set_linewidth(1.4)
    ax.spines["left"].set_linewidth(1.4)

    if show_legend and len(methods) > 1:
        try:
            from matplotlib.patches import Patch
        except Exception:
            Patch = None
        if Patch is not None:
            legend_items = []
            for method in methods:
                if method == "":
                    continue
                legend_items.append(
                    Patch(
                        facecolor=method_colors.get(method, "#F0F0F0"),
                        edgecolor="#2E3440",
                        label=method,
                    )
                )
            if legend_items:
                ax.legend(
                    handles=legend_items,
                    frameon=False,
                    loc="upper center",
                    bbox_to_anchor=(0.5, 1.15),
                    ncol=len(legend_items),
                )

    fig.subplots_adjust(left=0.12, right=0.98, bottom=0.18, top=0.86)
    fig.savefig(out_path, dpi=150, bbox_inches="tight", pad_inches=0.02)
    print(f"wrote {out_path}")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Plot tool_memory_latency summary CSVs."
    )
    ap.add_argument(
        "--input_dir",
        default=".",
        help="directory containing latency CSVs (default: .)",
    )
    ap.add_argument(
        "--out_dir",
        default=".",
        help="output directory (default: .)",
    )
    ap.add_argument(
        "--label",
        action="store_true",
        help="show legend labels for flush method (if present)",
    )
    ap.add_argument(
        "--pdf",
        action="store_true",
        help="write outputs as PDF (default: PNG)",
    )
    args = ap.parse_args()

    in_dir = args.input_dir
    out_dir = args.out_dir
    if not os.path.isdir(in_dir):
        print(f"missing input_dir: {in_dir}", file=sys.stderr)
        return 1
    os.makedirs(out_dir, exist_ok=True)

    grouped = defaultdict(lambda: defaultdict(dict))
    for name in os.listdir(in_dir):
        if not name.endswith(".csv"):
            continue
        test, config = detect_test(name[:-4])
        if test is None:
            continue
        path = os.path.join(in_dir, name)
        grouped[test][config] = read_latency_csv(path)

    if not grouped:
        print("no matching CSVs found", file=sys.stderr)
        return 1

    rc = 0
    for test in TESTS:
        if test not in grouped:
            continue
        ext = "pdf" if args.pdf else "png"
        out_path = os.path.join(out_dir, f"latency_{test}.{ext}")
        rc |= plot_test(test, grouped[test], out_path, args.label)
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
