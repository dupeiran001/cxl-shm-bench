#!/usr/bin/env python3
import argparse
import csv
import math
import os
import re
import sys
from collections import defaultdict


TESTS = ["rd", "ntwr"]
PALETTE_BLUE = "#4C78A8"
PALETTE_ORANGE = "#F58518"
PALETTE_GREEN = "#54A24B"
#PALETTE_RED = "#bf616a"
#PALETTE_RED_LIGHT = "#D79DA3"
PALETTE_RED       = "#E45756"   # Vega red (more saturated than Nord #BF616A)
PALETTE_RED_LIGHT = "#ED9291"   # light tint that still reads well as a line
#PALETTE_RED       = "#D62728"   # matplotlib tab:red
#PALETTE_RED_LIGHT = "#E47373"
#PALETTE_RED       = "#FF3B30"   # vivid system-red style
#PALETTE_RED_LIGHT = "#FF8078"
#PALETTE_RED       = "#C1121F"
#PALETTE_RED_LIGHT = "#D7656D"


PALETTE_PURPLE = "#B279A2"
PALETTE_BLUE_LIGHT = "#76A5D5"
PALETTE_ORANGE_LIGHT = "#F7A75B"
SPARSE_Y_PREFIXES = {
    "dram",
    "local_cxl",
    "localcxl",
    "remote_cxl",
    "2host_1cxl",
    "2host_1_cxl",
    "2host1cxl",
}
SMALL_HEADROOM_PREFIXES = {
    "2host_4cxl",
    "2host_4_cxl",
    "2host4cxl",
    "4host_8cxl",
    "4host_8_cxl",
    "4host8cxl",
}


def read_csv(path):
    rows = []
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows.append(row)
    return rows


def read_pointer_chasing(path):
    if not os.path.exists(path):
        return {}
    rows = []
    with open(path, newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            rows.append(row)
    results = {}
    for row in rows:
        test = (row.get("test") or "").strip()
        raw = row.get("bw(MBps)") or row.get("bw") or ""
        threads_raw = row.get("threads") or row.get("concurrency") or row.get("parallel") or ""
        if not test:
            continue
        try:
            val = float(str(raw).strip())
        except Exception:
            continue
        threads = None
        if threads_raw is not None and str(threads_raw).strip() != "":
            try:
                threads = int(str(threads_raw).strip())
            except Exception:
                threads = None
        results[test] = {"bw_mbps": val, "threads": threads}
    return results


def coerce_int(val):
    try:
        return int(val)
    except Exception:
        return None


def coerce_float(val):
    try:
        return float(val)
    except Exception:
        return None


def _std(vals):
    if len(vals) < 2:
        return 0.0
    mean = sum(vals) / len(vals)
    return math.sqrt(sum((v - mean) ** 2 for v in vals) / (len(vals) - 1))


def reduce_rows(rows):
    """Returns (mean_map, std_map): parallel -> value / spread across repeated
    runs. Repeated rows for the same (parallel, host) are treated as
    independent runs; multi-host files use the coordinator's 'avg' rows scaled
    by the distinct host count (aggregate bandwidth)."""
    grouped = defaultdict(list)
    for row in rows:
        parallel = coerce_int(row.get("parallel", ""))
        speed = coerce_float(row.get("speed_mb_s", ""))
        host = row.get("host", "")
        if parallel is None or speed is None:
            continue
        grouped[(parallel, host)].append(speed)

    has_avg = any(h == "avg" for (_, h) in grouped.keys())
    host_counts = defaultdict(int)
    for (parallel, host) in grouped.keys():
        if host != "avg":
            host_counts[parallel] += 1

    final = defaultdict(float)
    variance = defaultdict(float)
    for (parallel, host), speeds in grouped.items():
        mean = sum(speeds) / len(speeds)
        if has_avg:
            if host == "avg":
                count = host_counts.get(parallel, 0) or 1
                final[parallel] = mean * count
                variance[parallel] = (_std(speeds) * count) ** 2
        else:
            final[parallel] += mean
            variance[parallel] += _std(speeds) ** 2
    spread = {p: math.sqrt(v) for p, v in variance.items()}
    return final, spread


def format_prefix_label(prefix: str) -> str:
    label = prefix.replace("_", " ")
    label = re.sub(r"(\d)(host)", r"\1 host", label)
    label = re.sub(r"(\d)(cxl)", r"\1 cxl", label)
    label = re.sub(r"(\d)(dax)", r"\1 dax", label)
    return label


def main():
    ap = argparse.ArgumentParser(
        description="Plot bw_mem results for a prefix (rd/wr/ntwr)."
    )
    ap.add_argument("prefix", help="file prefix (e.g., dram, local_dax)")
    ap.add_argument(
        "--overlay",
        action="append",
        default=[],
        help="additional prefix to overlay (can repeat)",
    )
    ap.add_argument(
        "--out",
        default=None,
        help="output image path (default: <prefix>.png)",
    )
    ap.add_argument(
        "--pdf",
        action="store_true",
        help="write output as PDF (default: PNG)",
    )
    ap.add_argument("--title", default=None, help="plot title")
    ap.add_argument(
        "--label",
        action="store_true",
        help="show legend labels for line/color/marker",
    )
    args = ap.parse_args()

    prefix = args.prefix
    prefix_key = prefix.lower()
    out = args.out or f"{prefix}.png"
    if args.pdf:
        if out.endswith(".png"):
            out = out[:-4] + ".pdf"
        elif not out.endswith(".pdf"):
            out = out + ".pdf"
    title = args.title or ""
    pointer_map = read_pointer_chasing("pointer_chasing.csv")
    pointer_entry = pointer_map.get(prefix)
    pointer_mbps = None
    pointer_threads = None
    if pointer_entry:
        pointer_mbps = pointer_entry.get("bw_mbps")
        pointer_threads = pointer_entry.get("threads")

    def load_series(prefix_value):
        series = {}
        for test in TESTS:
            path = f"{prefix_value}_{test}.csv"
            if not os.path.exists(path):
                print(f"missing: {path}", file=sys.stderr)
                return None
            rows = read_csv(path)
            series[test] = reduce_rows(rows)
        return series

    datasets = []
    base_series = load_series(prefix)
    if base_series is None:
        return 1
    datasets.append(
        {
            "prefix": prefix,
            "label": format_prefix_label(prefix),
            "series": base_series,
            "colors": {
                "rd": PALETTE_BLUE,
                "ntwr": PALETTE_RED,
                "wr": PALETTE_GREEN,
            },
            "legend_group_label": None,
            "legend_suffix": "",
            "line_dash": None,
        }
    )
    for overlay_prefix in args.overlay:
        overlay_series = load_series(overlay_prefix)
        if overlay_series is None:
            return 1
        overlay_key = overlay_prefix.lower()
        if overlay_key in {"dax_1host_4cxl", "dax_1host_4_cxl", "dax_1host4cxl"}:
            overlay_colors = {
                "rd": PALETTE_BLUE_LIGHT,
                "ntwr": PALETTE_RED_LIGHT,
                "wr": PALETTE_GREEN,
            }
            overlay_dash = (0, (4, 2))
        else:
            overlay_colors = {
                "rd": PALETTE_BLUE,
                "ntwr": PALETTE_RED,
                "wr": PALETTE_GREEN,
            }
            overlay_dash = None
        datasets.append(
            {
                "prefix": overlay_prefix,
                "label": format_prefix_label(overlay_prefix),
                "series": overlay_series,
                "colors": overlay_colors,
                "legend_group_label": None,
                "legend_suffix": "",
                "line_dash": overlay_dash,
            }
        )
    if prefix_key == "dax_remote":
        for dataset in datasets[1:]:
            overlay_key = dataset["prefix"].lower()
            if overlay_key in {"dax_1host_4cxl", "dax_1host_4_cxl", "dax_1host4cxl"}:
                datasets[0]["legend_suffix"] = ""
                dataset["legend_suffix"] = " (4CXL)"
                break

    try:
        import matplotlib.pyplot as plt
        from matplotlib.ticker import MultipleLocator
    except Exception as exc:
        print(f"matplotlib required: {exc}", file=sys.stderr)
        return 1

    plt.rcParams.update(
        {
            "font.size": 15,
            "axes.labelsize": 15,
            "xtick.labelsize": 13,
            "ytick.labelsize": 13,
            "legend.fontsize": 13,
        }
    )
    fig, ax = plt.subplots(figsize=(3.2, 3.0))
    styles = {
        "rd": {"linestyle": "-", "marker": "o"},
        "wr": {"linestyle": "-", "marker": "s"},
        "ntwr": {"linestyle": "-", "marker": "x"},
    }
    show_legend = args.label or len(datasets) > 1
    for dataset in datasets:
        for test in TESTS:
            data, err = dataset["series"].get(test, ({}, {}))
            if not data:
                continue
            xs = sorted(data.keys())
            ys = [data[x] / 1024.0 for x in xs]
            yerr = [err.get(x, 0.0) / 1024.0 for x in xs]
            style = dict(styles.get(test, {}))
            line_dash = dataset.get("line_dash")
            if line_dash is not None:
                style["linestyle"] = line_dash
            color = dataset["colors"].get(test, "#2E3440")
            label = None
            if show_legend:
                if len(datasets) == 1:
                    label = test
                else:
                    label = f"{test}{dataset.get('legend_suffix', '')}"
            ax.errorbar(
                xs,
                ys,
                yerr=yerr if any(e > 0 for e in yerr) else None,
                label=label,
                color=color,
                linewidth=3.0,
                markersize=8,
                markerfacecolor="none",
                markeredgecolor=color,
                markeredgewidth=1.4,
                capsize=3,
                elinewidth=1.2,
                **style,
            )

    if title:
        ax.set_title(title)
    ax.set_xlabel("Concurrency")
    ax.set_ylabel("Bandwidth (GB/s)")
    xticks = sorted(
        {
            x
            for dataset in datasets
            for test in TESTS
            for x in dataset["series"].get(test, ({}, {}))[0].keys()
        }
    )
    ax.set_xscale("log", base=2)
    ax.set_xticks(xticks)
    ax.set_xticklabels([str(x) for x in xticks])
    max_gbps = 0.0
    for dataset in datasets:
        for test in TESTS:
            for val in dataset["series"].get(test, ({}, {}))[0].values():
                max_gbps = max(max_gbps, val / 1024.0)
    if pointer_mbps is not None:
        max_gbps = max(max_gbps, pointer_mbps / 1024.0)
    step_candidates = [2, 5, 10, 50]
    step = 50
    if max_gbps > 0:
        best_step = None
        best_dist = None
        for cand in step_candidates:
            ticks = max_gbps / cand if cand > 0 else 0
            if 3 <= ticks <= 8:
                best_step = cand
                break
            dist = abs(ticks - 5)
            if best_dist is None or dist < best_dist:
                best_dist = dist
                best_step = cand
        step = best_step if best_step is not None else step
    if prefix_key in SPARSE_Y_PREFIXES:
        major_step = step * 2
        minor_step = step
    else:
        major_step = step
        minor_step = step / 2.0 if step > 1 else step
    ax.yaxis.set_major_locator(MultipleLocator(major_step))
    ax.yaxis.set_minor_locator(MultipleLocator(minor_step))
    if max_gbps <= 0:
        top = major_step * 2
    else:
        if prefix_key in SMALL_HEADROOM_PREFIXES:
            target = max_gbps * 1.3
        else:
            target = max_gbps + 1.5 * step
        top = math.ceil(target / major_step) * major_step
    top = max(top, 2 * major_step)
    ax.set_ylim(bottom=0, top=top)
    ax.grid(True, axis="y", which="major", linestyle=(0, (4, 6)), alpha=0.5, linewidth=0.9)
    if pointer_mbps is not None and xticks:
        pointer_gbps = pointer_mbps / 1024.0
        px = pointer_threads if pointer_threads is not None else max(xticks)
        ax.scatter(
            [px],
            [pointer_gbps],
            color=PALETTE_ORANGE,
            s=35,
            zorder=3.5,
        )
        label = f"non_seq ({pointer_gbps:.1f}G)"
        if prefix_key == "dram":
            xytext = (-28, 16)
            ha = "left"
            va = "bottom"
        else:
            xytext = (-12, 12)
            ha = "right"
            va = "bottom"
        ax.annotate(
            label,
            xy=(px, pointer_gbps),
            xytext=xytext,
            textcoords="offset points",
            ha=ha,
            va=va,
            color=PALETTE_ORANGE,
            fontsize=13,
            arrowprops=dict(arrowstyle="->", color=PALETTE_ORANGE, lw=1.2),
        )
    if show_legend:
        overlay_suffixes = {dataset.get("legend_suffix") for dataset in datasets}
        if prefix_key == "dax_remote" and " (4CXL)" in overlay_suffixes:
            legend_cols = 2
        else:
            legend_cols = len(TESTS)
        ax.legend(
            loc="upper center",
            bbox_to_anchor=(0.55, 1.15),
            ncol=legend_cols,
            frameon=False,
            columnspacing=0.8,
            handletextpad=0.5,
        )
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["bottom"].set_linewidth(1.4)
    ax.spines["left"].set_linewidth(1.4)
    ax.tick_params(axis="both", width=1.2)

    fig.subplots_adjust(left=0.14, right=0.98, bottom=0.22, top=0.86)
    fig.savefig(out, dpi=200, bbox_inches="tight", pad_inches=0.01)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
