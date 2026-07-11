#!/usr/bin/env python3
"""Plot hashtable_mh throughput: mean aggregate Mops across runs, grouped by
memory configuration x read/write mix (one compact figure)."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


# Same config palette/hatching as gapbs_mh/scripts/plot_gapbs.py.
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

CONFIG_BAR_STYLE = {
    "dram": {"facecolor": "white", "hatch": ""},
    "remote_cxl": {"facecolor": LIGHT_BLUE, "hatch": "\\\\\\"},
    "shared_2hosts": {"facecolor": LIGHT_ORANGE, "hatch": "///"},
    "shared_4hosts": {"facecolor": LIGHT_GREEN, "hatch": "xxx"},
}

MIX_ORDER = [100, 95, 50]


def mix_label(mix: int) -> str:
    if mix == 100:
        return "100%R"
    return f"{mix}R/{100 - mix}W"


def plot(df: pd.DataFrame, threads: int, out_dir: Path) -> Path:
    sdf = df[df["threads_per_host"].astype(int) == threads].copy()
    if sdf.empty:
        raise SystemExit(f"no rows with threads_per_host={threads}")
    sdf["mops"] = pd.to_numeric(sdf["mops"], errors="coerce")
    sdf = sdf.dropna(subset=["mops"])

    plt.rcParams.update(
        {
            "font.size": 16,
            "axes.labelsize": 16,
            "xtick.labelsize": 15,
            "ytick.labelsize": 15,
            "legend.fontsize": 14,
            "axes.linewidth": 1.4,
            "xtick.major.width": 1.4,
            "ytick.major.width": 1.4,
            "xtick.major.size": 5.0,
            "ytick.major.size": 5.0,
            "hatch.linewidth": 1.2,
        }
    )

    mixes = [m for m in MIX_ORDER if m in set(sdf["mix"].astype(int))]
    if not mixes:
        mixes = sorted(set(sdf["mix"].astype(int)), reverse=True)

    fig, ax = plt.subplots(figsize=(5.6, 4.2))
    n_cfg = len(CONFIG_ORDER)
    group_width = 0.88
    bar_span = group_width / n_cfg
    bar_width = bar_span * 0.86
    x_positions = list(range(len(mixes)))

    for i_cfg, cfg in enumerate(CONFIG_ORDER):
        cdf = sdf[sdf["benchmark"] == cfg]
        if cdf.empty:
            continue
        # Repeated reproduction runs append duplicate (config, mix) rows:
        # aggregate to mean, error bars show the spread across runs.
        agg = cdf.groupby(cdf["mix"].astype(int))["mops"].agg(["mean", "std"])
        xs, ys, es = [], [], []
        for i_mix, mix in enumerate(mixes):
            if mix not in agg.index:
                continue
            xs.append(x_positions[i_mix] + (i_cfg - (n_cfg - 1) / 2) * bar_span)
            ys.append(float(agg.loc[mix, "mean"]))
            std = agg.loc[mix, "std"]
            es.append(0.0 if pd.isna(std) else float(std))
        if not xs:
            continue
        style = CONFIG_BAR_STYLE.get(cfg, {"facecolor": "white", "hatch": ""})
        ax.bar(
            xs,
            ys,
            width=bar_width,
            yerr=es if any(e > 0 for e in es) else None,
            capsize=2.5,
            error_kw={"elinewidth": 1.0, "zorder": 4},
            label=CONFIG_LABEL.get(cfg, cfg),
            edgecolor=EDGE_COLOR,
            linewidth=0.9,
            zorder=3,
            **style,
        )

    ax.set_xlabel(f"Workload mix ({threads} threads/host)")
    ax.set_ylabel("Aggregate throughput (Mops/s)")
    ax.grid(axis="y", linestyle="--", linewidth=0.8, alpha=0.5, zorder=0)
    ax.legend(
        frameon=False,
        ncols=2,
        loc="lower center",
        bbox_to_anchor=(0.5, 1.0),
        columnspacing=0.9,
        handletextpad=0.5,
    )
    ax.set_xticks(x_positions, [mix_label(m) for m in mixes])
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)

    fig.tight_layout()
    out_dir.mkdir(parents=True, exist_ok=True)
    base = out_dir / f"fig_ht_mops_t{threads}"
    pdf_path = base.with_suffix(".pdf")
    fig.savefig(pdf_path, bbox_inches="tight")
    fig.savefig(base.with_suffix(".png"), bbox_inches="tight", dpi=300)
    plt.close(fig)
    return pdf_path


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Plot hashtable_mh results.")
    ap.add_argument(
        "--csv",
        default=str(script_dir / ".." / "result" / "raw" / "hashtable.csv"),
        help="Input CSV (default: ../result/raw/hashtable.csv)",
    )
    ap.add_argument(
        "--out_dir",
        default=str((script_dir / ".." / "result" / "plots").resolve()),
        help="Output directory (default: ../result/plots/)",
    )
    ap.add_argument(
        "--threads",
        type=int,
        default=0,
        help="threads_per_host to plot (default: max present in the CSV)",
    )
    args = ap.parse_args()

    df = pd.read_csv(args.csv)
    threads = args.threads or int(df["threads_per_host"].astype(int).max())
    pdf_path = plot(df, threads, Path(args.out_dir))
    print(pdf_path)
    print(pdf_path.with_suffix(".png"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
