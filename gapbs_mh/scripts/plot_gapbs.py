#!/usr/bin/env python3

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


CONFIG_ORDER = ["dram", "remote_cxl", "shared_2hosts", "shared_4hosts"]
CONFIG_LABEL = {
    "dram": "Local DRAM",
    "remote_cxl": "Remote CXL",
    "shared_2hosts": "Shared (2 hosts)",
    "shared_4hosts": "Shared (4 hosts)",
}

EDGE_COLOR = "#2E3440"
LIGHT_BLUE = "#BFD4EA"
LIGHT_ORANGE = "#FCD6A5"
LIGHT_GREEN = "#C8E6C4"

# Match the light blue/orange bar styling used under benchmark/result/.
CONFIG_BAR_STYLE = {
    "dram": {"facecolor": "white", "hatch": ""},
    # Remote: blue + light-gray backslash hatch; Shared-2h: orange + light-gray slash; Shared-4h: green solid
    "remote_cxl": {"facecolor": LIGHT_BLUE, "hatch": "\\\\"},
    "shared_2hosts": {"facecolor": LIGHT_ORANGE, "hatch": "//"},
    "shared_4hosts": {"facecolor": LIGHT_GREEN, "hatch": ""},
}


def _bench_filename_token(bench: str) -> str:
    return bench.lower().replace(" ", "_").replace("-", "_")

def estimate_graph_wss_gib(g: int, k: int) -> float:
    """
    Rough CSR WSS estimate (GiB) for GAPBS synthetic graphs.

    GAPBS prints "undirected edges" but stores CSR neighbor lists as directed
    edges for traversal; empirically `num_edges` is close to N*k, and the CSR
    uses 2*num_edges neighbor entries for undirected graphs.

    Estimate:
      N = 2^g
      offsets: (N+1) * 8 bytes (SGOffset = int64)
      neighbors: (2 * N * k) * 4 bytes (NodeID = int32) == 8*N*k bytes
    """
    n = 1 << int(g)
    offsets_bytes = (n + 1) * 8
    neighbors_bytes = 8 * n * int(k)
    return (offsets_bytes + neighbors_bytes) / (1024.0**3)

def estimate_benchmark_state_wss_gib(bench: str, g: int) -> float:
    """
    Estimate per-benchmark state arrays beyond CSR.

    Intentionally ignores small synchronization metadata. For BFS this includes
    `parent[]` and two frontier bitmaps.
    """
    n = 1 << int(g)
    bench = bench.upper()

    if bench == "BFS":
        parent_bytes = 4 * n  # NodeID=int32
        frontier_bytes = (n + 7) // 8
        # Two bitmaps that alternate (frontier0/frontier1).
        return (parent_bytes + 2 * frontier_bytes) / (1024.0**3)

    if bench in {"PR", "PR-SPMV"}:
        # scores[] + outgoing[] (float each).
        return (8 * n) / (1024.0**3)

    if bench == "TC":
        return 0.0

    return 0.0

def estimate_benchmark_wss_gib(bench: str, g: int, k: int) -> float:
    return estimate_graph_wss_gib(g, k) + estimate_benchmark_state_wss_gib(bench, g)

def wss_token_gib(wss_gib: float) -> str:
    # Avoid '.' in filenames.
    return f"{wss_gib:.1f}".replace(".", "p")


def plot_one_benchmark(df: pd.DataFrame, bench: str, out_dir: Path) -> Path:
    sdf = df[df["benchmark"] == bench].copy()
    if sdf.empty:
        raise SystemExit(f"no rows for benchmark={bench!r}")

    # Assume one (g,k) working set per benchmark.
    g_vals = sorted(set(int(x) for x in sdf["g"].tolist()))
    k_vals = sorted(set(int(x) for x in sdf["k"].tolist()))
    if len(g_vals) != 1 or len(k_vals) != 1:
        raise SystemExit(
            f"benchmark={bench!r} has multiple (g,k): g={g_vals} k={k_vals}"
        )
    g = g_vals[0]
    k = k_vals[0]
    wss_gib = estimate_benchmark_wss_gib(bench, g, k)

    plt.rcParams.update(
        {
            "font.size": 16,
            "axes.labelsize": 16,
            "xtick.labelsize": 15,
            "ytick.labelsize": 15,
            "legend.fontsize": 16,
            "axes.linewidth": 1.4,
            "xtick.major.width": 1.4,
            "ytick.major.width": 1.4,
            "xtick.major.size": 5.0,
            "ytick.major.size": 5.0,
            "hatch.linewidth": 0.8,
            "hatch.color": "#B0B0B0",
        }
    )

    sdf["avg_s"] = pd.to_numeric(sdf["avg_s"], errors="coerce")
    sdf = sdf.dropna(subset=["avg_s"])

    xticks = sorted(set(int(x) for x in sdf["total_threads"].tolist()))
    fig, ax = plt.subplots(figsize=(5.6, 4.2))

    n_cfg = len(CONFIG_ORDER)
    group_width = 0.88
    bar_span = group_width / n_cfg
    # Make bars slightly narrower than their slots so adjacent bar borders
    # don't visually fuse into an extra-thick line.
    bar_width = bar_span * 0.86
    x_positions = list(range(len(xticks)))

    for i_cfg, cfg in enumerate(CONFIG_ORDER):
        cdf = sdf[sdf["config"] == cfg].copy()
        if cdf.empty:
            continue
        # Repeated reproduction runs append duplicate (config, threads) rows:
        # aggregate to mean, and show the spread across runs as error bars.
        agg = cdf.groupby(cdf["total_threads"].astype(int))["avg_s"].agg(["mean", "std"])
        y_by_threads = {int(t): float(row["mean"]) for t, row in agg.iterrows()}
        e_by_threads = {
            int(t): (0.0 if pd.isna(row["std"]) else float(row["std"]))
            for t, row in agg.iterrows()
        }
        xs = [x_positions[i] + (i_cfg - (n_cfg - 1) / 2) * bar_span for i in range(len(xticks))]
        xs_plot = []
        ys_plot = []
        es_plot = []
        for x, t in zip(xs, xticks):
            y = y_by_threads.get(t)
            if y is None:
                continue
            xs_plot.append(x)
            ys_plot.append(y)
            es_plot.append(e_by_threads.get(t, 0.0))
        style = CONFIG_BAR_STYLE.get(cfg, {"facecolor": "white", "hatch": ""})
        ax.bar(
            xs_plot,
            ys_plot,
            width=bar_width,
            yerr=es_plot if any(e > 0 for e in es_plot) else None,
            capsize=2.5,
            error_kw={"elinewidth": 1.0, "zorder": 4},
            label=CONFIG_LABEL.get(cfg, cfg),
            edgecolor=EDGE_COLOR,
            linewidth=0.9,
            zorder=3,
            **style,
        )

    ax.set_title(f"WSS \u2248 {wss_gib:.1f} GiB")
    ax.set_xlabel("Total threads")
    ax.set_ylabel("Runtime (s)")
    ax.grid(axis="y", linestyle="--", linewidth=0.8, alpha=0.5, zorder=0)
    ax.legend(frameon=False, ncols=1, loc="best", fontsize=14)

    ax.set_xticks(x_positions, [str(x) for x in xticks])
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_linewidth(1.4)
    ax.spines["bottom"].set_linewidth(1.4)

    fig.tight_layout()

    out_dir.mkdir(parents=True, exist_ok=True)
    base = out_dir / f"fig_gapbs_{_bench_filename_token(bench)}_wss_{wss_token_gib(wss_gib)}GiB"
    pdf_path = base.with_suffix(".pdf")
    png_path = base.with_suffix(".png")
    fig.savefig(pdf_path, bbox_inches="tight")
    fig.savefig(png_path, bbox_inches="tight", dpi=300)
    plt.close(fig)
    return pdf_path


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    default_csv = str(script_dir / ".." / "result" / "raw" / "gapbs.csv")
    default_out = str((script_dir / ".." / "result" / "plots").resolve())
    ap = argparse.ArgumentParser(description="Plot GAPBS results for paper.")
    ap.add_argument(
        "--csv",
        default=default_csv,
        help="Input CSV (default: ../result/raw/gapbs.csv relative to this script)",
    )
    ap.add_argument(
        "--out_dir",
        default=default_out,
        help="Output directory for figures (default: ../result/plots/ relative to this script)",
    )
    ap.add_argument(
        "--bench",
        action="append",
        default=[],
        help="Benchmark name to plot (repeatable). Default: plot all.",
    )
    args = ap.parse_args()

    df = pd.read_csv(args.csv)
    out_dir = Path(args.out_dir)

    benches = args.bench or sorted(set(df["benchmark"].tolist()))
    for bench in benches:
        pdf_path = plot_one_benchmark(df, bench, out_dir)
        print(pdf_path)
        print(pdf_path.with_suffix(".png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
