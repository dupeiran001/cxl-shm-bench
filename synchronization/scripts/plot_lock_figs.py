#!/usr/bin/env python3
"""
plot_locks.py

Auto-run script:
- Reads these files from ../result/raw/:
    run_all.csv
    run_all_central.csv

- Writes figures + parsed table to:
    ./fig/

Strict behavior:
- Any exit_code != 0 -> ERROR and exit(1)
- Any parsed error_count != 0 -> ERROR and exit(1)
- Any parsing mismatch / missing required data -> ERROR and exit(1)

Run:
    chmod +x plot_locks.py
    ./plot_locks.py
"""

import os, re, sys
import math
from pathlib import Path
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator, FuncFormatter, LogLocator, LogFormatterMathtext
from matplotlib.patches import Patch

BASE_TEXT_SIZE = 16
TICK_TEXT_SIZE = BASE_TEXT_SIZE - 1
LEGEND_TEXT_SIZE = BASE_TEXT_SIZE - 2
WHISKER_LW = 1.6
CAP_LW = 1.6
BAR_EDGE_LW = 1.2
BASE_FIGSIZE = (8.0, 6.0)
FIG_W_SCALE = 1
FIG_H_SCALE = 1
DEFAULT_FIGSIZE = (BASE_FIGSIZE[0] * FIG_W_SCALE, BASE_FIGSIZE[1] * FIG_H_SCALE)

# Per-figure size scaling (relative to BASE_FIGSIZE).
FIG1_FIG_W_SCALE = 1.1
FIG1_FIG_H_SCALE = 1
FIG4_FIG_W_SCALE = 0.9
FIG4_FIG_H_SCALE = 1
FIG1_FIGSIZE = (BASE_FIGSIZE[0] * FIG1_FIG_W_SCALE, BASE_FIGSIZE[1] * FIG1_FIG_H_SCALE)
FIG4_FIGSIZE = (BASE_FIGSIZE[0] * FIG4_FIG_W_SCALE, BASE_FIGSIZE[1] * FIG4_FIG_H_SCALE)
FIG134_TEXT_SCALE = 1.25
FIG2_TEXT_SCALE = 1.6

# Per-figure tuning knobs (keep Fig1/Fig4 independent for paper layout).
FIG1_TEXT_SCALE = 1.4
FIG1_TICK_SCALE = FIG1_TEXT_SCALE
FIG1_LEGEND_SCALE = 1.4
FIG1_LEGEND_ROWS = 2
FIG1_LEGEND_X = 0.55
FIG1_LEGEND_Y = 1.12
FIG1_MARKER_SCALE = 2.0
FIG1_LINE_LW = 4.0
FIG1_SPIN_LINE_LW = 4.0
FIG1_MARKER_EDGE_LW = 1.8
FIG1_SPIN_MARKER_EDGE_LW = 2.5

FIG4_TEXT_SCALE = 1.4
FIG4_TICK_SCALE = FIG4_TEXT_SCALE
FIG4_LEGEND_SCALE = 1.4
FIG4_LEGEND_ROWS = 2
FIG4_BAR_EDGE_LW = BAR_EDGE_LW
FIG4_WHISKER_LW = WHISKER_LW
FIG4_CAP_LW = CAP_LW
FIG4_BAR_ALPHA = 0.85
FIG4_MINOR_TICKS_SUBS = (2, 4, 6, 8)
FIG4_TICK_LEN_MAJOR = 6
FIG4_TICK_LEN_MINOR = 3

plt.rcParams.update({
    "figure.dpi": 120,
    "savefig.dpi": 240,
    "font.size": BASE_TEXT_SIZE,
    "axes.labelsize": BASE_TEXT_SIZE,
    "axes.titlesize": BASE_TEXT_SIZE,
    "legend.fontsize": LEGEND_TEXT_SIZE,
    "xtick.labelsize": TICK_TEXT_SIZE,
    "ytick.labelsize": TICK_TEXT_SIZE,
    "axes.linewidth": 1.4,
    "axes.axisbelow": True,
    "lines.linewidth": 1.6,
    "lines.markersize": 5.0,
    # Make hatch patterns lighter/thinner than bar borders.
    "hatch.color": "0.6",
    "hatch.linewidth": 0.7,
})

def die(msg: str):
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(1)

def legend_ncols_for_rows(n_items: int, n_rows: int) -> int:
    if n_rows <= 0:
        return n_items
    return max(1, int(math.ceil(n_items / n_rows)))

def print_lock_avg_tables(df: pd.DataFrame, outdir: Path):
    hosts = [1, 2, 3, 4]
    lock_counts = [1, 2, 4, 8]
    d = df[
        (df.memory == "cxl")
        & (df.hold_ns == 0)
        & (df.gap_ns == 0)
        & (df.channels == 1)
        & (df.threads == 1)
        & (df.hosts.isin(hosts))
        & (df.locks.isin(lock_counts))
    ].copy()
    if d.empty:
        print(
            "WARNING: no rows for avg tables "
            "(CXL, hosts in {1,2,3,4}, locks in {1,2,4,8}, threads=1, channels=1, hold=0, gap=0)",
            file=sys.stderr,
        )
        return

    long_tbl = (
        d.groupby(["lock_name", "hosts", "locks"], as_index=False)["lock_avg_ns"]
        .agg(n="count", median_ns="median", mean_ns="mean", min_ns="min", max_ns="max")
    )
    long_tbl["lock"] = long_tbl["lock_name"].map(DISPLAY_NAMES).fillna(long_tbl["lock_name"])
    long_tbl = long_tbl[["lock", "hosts", "locks", "n", "median_ns", "mean_ns", "min_ns", "max_ns"]]
    long_tbl = long_tbl.sort_values(["lock", "hosts", "locks"])

    long_tbl_round = long_tbl.copy()
    for c in ["median_ns", "mean_ns", "min_ns", "max_ns"]:
        long_tbl_round[c] = long_tbl_round[c].map(lambda x: float(f"{x:.2f}"))

    # Print readable per-lock matrices (hosts x locks) using median(avg).
    locks_in_data = [k for k in sort_lock_order(d["lock_name"].unique())]
    print("\n=== Lock avg summary (median lock_avg_ns, ns) ===")
    print("Filter: CXL, threads=1, channels=1, hold=0, gap=0")
    print(f"Hosts: {hosts}, Locks: {lock_counts}\n")
    for lock_name in locks_in_data:
        sub = long_tbl_round[long_tbl_round["lock"] == DISPLAY_NAMES.get(lock_name, lock_name)]
        if sub.empty:
            continue
        mat = (
            sub.pivot(index="hosts", columns="locks", values="median_ns")
            .reindex(index=hosts, columns=lock_counts)
        )
        disp = DISPLAY_NAMES.get(lock_name, lock_name)
        print(f"[{disp}] median(lock_avg_ns) (ns)")
        # Avoid scientific notation; show blanks for missing.
        print(mat.to_string(index=True, na_rep=""))
        print("")
    print("==============================================\n")

def require_cols(df: pd.DataFrame, cols, where: str):
    missing = [c for c in cols if c not in df.columns]
    if missing:
        die(f"{where}: missing columns: {missing}")

def extract_lock_from_cmd(cmd: str):
    m = re.search(r'--lock\s+(\S+)', cmd)
    return m.group(1) if m else None

def extract_int_opt(cmd: str, opt: str):
    m = re.search(rf'--{re.escape(opt)}\s+([0-9]+)', cmd)
    return int(m.group(1)) if m else np.nan

def parse_stdout(stdout: str):
    if not isinstance(stdout, str):
        die("stdout is not a string")

    def get_int(pattern: str):
        m = re.search(pattern, stdout)
        if not m:
            die(f"stdout missing pattern: {pattern}")
        return int(m.group(1))

    def get_float(pattern: str):
        m = re.search(pattern, stdout)
        if not m:
            die(f"stdout missing pattern: {pattern}")
        return float(m.group(1))

    m = re.search(r'hosts/host_id:\s*(\d+)\s*/\s*(\d+)', stdout)
    if not m:
        die("stdout missing hosts/host_id")
    hosts = int(m.group(1))
    host_id = int(m.group(2))

    threads = get_int(r'threads:\s*(\d+)')
    locks = get_int(r'locks:\s*(\d+)')
    channels = get_int(r'channels:\s*(\d+)')
    seconds = get_float(r'seconds:\s*([0-9.]+)')

    total_ops = get_int(r'total ops:\s*(\d+)')
    total_time_ms = get_float(r'total time:\s*([0-9.]+)\s*ms')

    lock_avg_ns = get_float(r'lock avg:\s*([0-9.]+)\s*ns')
    m = re.search(r'lock p50/p95/p99:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)\s*ns', stdout)
    if not m:
        die("stdout missing lock p50/p95/p99")
    lock_p50_ns, lock_p95_ns, lock_p99_ns = map(int, m.groups())

    unlock_avg_ns = get_float(r'unlock avg:\s*([0-9.]+)\s*ns')
    m = re.search(r'unlock p50/p95/p99:\s*(\d+)\s*/\s*(\d+)\s*/\s*(\d+)\s*ns', stdout)
    if not m:
        die("stdout missing unlock p50/p95/p99")
    unlock_p50_ns, unlock_p95_ns, unlock_p99_ns = map(int, m.groups())

    total_retries = get_int(r'total retries:\s*(\d+)')
    avg_retries_per_op = get_float(r'avg retries/op:\s*([0-9.]+)')

    total_poll_loops = get_int(r'total poll loops:\s*(\d+)')
    avg_poll_loops_per_op = get_float(r'avg poll loops/op:\s*([0-9.]+)')

    global_counter = get_int(r'global counter:\s*(\d+)')
    error_count = get_int(r'error count:\s*(\d+)')

    return dict(
        hosts=hosts, host_id=host_id, threads=threads, locks=locks, channels=channels,
        seconds=seconds, total_ops=total_ops, total_time_ms=total_time_ms,
        lock_avg_ns=lock_avg_ns, lock_p50_ns=lock_p50_ns, lock_p95_ns=lock_p95_ns, lock_p99_ns=lock_p99_ns,
        unlock_avg_ns=unlock_avg_ns, unlock_p50_ns=unlock_p50_ns, unlock_p95_ns=unlock_p95_ns, unlock_p99_ns=unlock_p99_ns,
        total_retries=total_retries, avg_retries_per_op=avg_retries_per_op,
        total_poll_loops=total_poll_loops, avg_poll_loops_per_op=avg_poll_loops_per_op,
        global_counter=global_counter, error_count=error_count
    )

def parse_runlog_csv(path: str, memory: str) -> pd.DataFrame:
    if not os.path.exists(path):
        die(f"missing file: {path}")
    raw = pd.read_csv(path)
    require_cols(raw, ['timestamp_utc','group','cmd_index','host','command','exit_code','duration_s','stdout'], path)
    if (raw['exit_code'].astype(int) != 0).any():
        bad = raw.loc[raw['exit_code'].astype(int)!=0, ['cmd_index','host','exit_code']].head(5).to_dict('records')
        die(f"{path}: found non-zero exit_code rows (showing up to 5): {bad}")

    recs = []
    for i, row in raw.iterrows():
        cmd = row['command']
        lock_name = extract_lock_from_cmd(cmd) if isinstance(cmd, str) else None
        if lock_name is None:
            die(f"{path}: cannot parse --lock from command at row {i}")

        hold_ns = extract_int_opt(cmd, "hold_ns")
        gap_ns  = extract_int_opt(cmd, "gap_ns")

        parsed = parse_stdout(row['stdout'])
        if parsed['error_count'] != 0:
            die(f"{path}: error_count != 0 at row {i}: {parsed['error_count']}")

        recs.append({
            "timestamp_utc": row["timestamp_utc"],
            "group": int(row["group"]),
            "cmd_index": int(row["cmd_index"]),
            "host": str(row["host"]),
            "command": cmd,
            "duration_s": float(row["duration_s"]),
            "memory": memory,
            "lock_name": lock_name,
            "hold_ns": int(hold_ns) if not np.isnan(hold_ns) else np.nan,
            "gap_ns": int(gap_ns) if not np.isnan(gap_ns) else np.nan,
            **parsed
        })
    df = pd.DataFrame.from_records(recs)
    # Outlier guard: a run whose first acquisition blocked on a startup race
    # (e.g. the centralized lock-server coming up) reports a tail latency
    # orders of magnitude above its config's other repetitions, which both
    # skews whiskers and explodes the axis tick locator. Drop rows whose
    # lock_p95_ns exceeds 20x the median p95 of the same (lock, hosts, locks)
    # config, loudly, keeping the raw CSV untouched.
    if not df.empty and "lock_p95_ns" in df.columns:
        med = df.groupby(["lock_name", "hosts", "locks"])["lock_p95_ns"].transform("median")
        bad = df["lock_p95_ns"] > 20 * med
        for _, r in df[bad].iterrows():
            print(f"WARN: dropping outlier row (startup artifact?): "
                  f"{r['lock_name']} hosts={r['hosts']} locks={r['locks']} "
                  f"p95={r['lock_p95_ns']/1e6:.1f} ms (20x median rule)",
                  file=sys.stderr)
        df = df[~bad]
    return df

DISPLAY_NAMES = {"spin":"Spin","lfm":"LFM","peterson":"Peterson","bakery":"Bakery","centralized":"Centralized"}

# Nord (Aurora) palette subset.
# - Blue:   nord10  #5E81AC
# - Green:  nord14  #A3BE8C
# - Yellow: nord13  #EBCB8B
# - Purple: nord15  #B48EAD
CB = {
    "blue": "#5E81AC",
    "green": "#A3BE8C",
    "yellow": "#EBCB8B",
    "purple": "#B48EAD",
    "black": "#000000",
}

GRAY_808080 = "#505050"

ALG_STYLE = {
    "lfm": {"color": "#4C78A8", "ls": "-", "hatch": "///", "marker": "o"},
    "peterson": {"color": "#E45756", "ls": "--", "hatch": "\\\\", "marker": "s"},
    "bakery": {"color": "#72B7B2", "ls": "-.", "hatch": "xx", "marker": "D"},
    "centralized": {"color": "#B279A2", "ls": ":", "hatch": "..", "marker": "^"},
    # Spin is kept black to avoid reusing one of the 4 lock colors.
    "spin": {"color": CB["black"], "ls": "--", "hatch": "++", "marker": "x"},
}

LOCKCOUNT_STYLE = {
    1: {"color": CB["blue"], "ls": "-", "hatch": "///", "marker": "o"},
    2: {"color": CB["purple"], "ls": "--", "hatch": "\\\\", "marker": "s"},
    4: {"color": CB["green"], "ls": "-.", "hatch": "xx", "marker": "D"},
    8: {"color": CB["yellow"], "ls": ":", "hatch": "..", "marker": "^"},
}

# Bar styles for 4 bars in one x-group (paper-friendly).
# Intended order: lock_counts=[1,2,4,8] -> [white, gray, very-light blue, very-light yellow].
FOUR_BAR_STYLE = {
    1: {"facecolor": "white", "hatch": ""},
    2: {"facecolor": GRAY_808080, "hatch": ""},       # gray
    4: {"facecolor": "#BFD4EA", "hatch": "\\\\\\"},    # light blue + \\\
    8: {"facecolor": "#FCD6A5", "hatch": "///"},      # lighter orange + ///
}

def sort_lock_order(keys):
    order = ["spin","lfm","peterson","bakery","centralized"]
    return [k for k in order if k in keys] + [k for k in keys if k not in order]

def fmt_range(values) -> str:
    vals = sorted({int(v) for v in values if not pd.isna(v)})
    if not vals:
        return "none"
    if len(vals) == 1:
        return str(vals[0])
    return f"{vals[0]}-{vals[-1]}"

def savefig(fig, outdir: Path, name: str):
    p_png = outdir / f"{name}.png"
    p_pdf = outdir / f"{name}.pdf"
    supt = getattr(fig, "_suptitle", None)
    if supt is not None:
        supt.set_visible(False)
    for ax in fig.axes:
        ax.set_title("")
        # Paper-friendly axes chrome:
        # - Always hide the top spine.
        # - Hide the right spine unless this axis is a right-side y-axis.
        ax.spines["top"].set_visible(False)
        is_right_axis = (ax.yaxis.get_ticks_position() == "right") or (ax.yaxis.get_label_position() == "right")
        if not is_right_axis:
            ax.spines["right"].set_visible(False)
    # tight_layout tends to fight with broken-axis multi-axes figures; bbox_inches="tight" is enough.
    if len(fig.axes) == 1:
        try:
            fig.tight_layout(pad=0.2)
        except Exception:
            pass
    save_kws = dict(bbox_inches="tight", pad_inches=0.02)
    fig.savefig(p_png, dpi=240, **save_kws)
    fig.savefig(p_pdf, **save_kws)
    print(f"saved {p_png}")
    print(f"saved {p_pdf}")
    plt.close(fig)
    return p_png

def main():
    script_dir = Path(__file__).resolve().parent
    raw_dir = script_dir / ".." / "result" / "raw"
    outdir = (script_dir / ".." / "result" / "plots").resolve()

    parts = [
        parse_runlog_csv(str(raw_dir / "run_all.csv"), "cxl"),
        parse_runlog_csv(str(raw_dir / "run_all_central.csv"), "cxl"),
    ]
    df = pd.concat(parts, ignore_index=True)

    outdir.mkdir(parents=True, exist_ok=True)
    print_lock_avg_tables(df, outdir)

    # ---------- FIG 1: merged p50 with separate y-scales (distributed vs spin) ----------
    d_hosts = df[(df.memory=="cxl") & (df.hold_ns==0) & (df.gap_ns==0) & (df.threads==1) & (df.locks==1) & (df.channels==1)]
    d_spin  = df[(df.memory=="cxl") & (df.hold_ns==0) & (df.gap_ns==0) & (df.hosts==1) & (df.locks==1) & (df.channels==1) & (df.lock_name=="spin")]

    hosts = sorted(d_hosts["hosts"].unique())
    procs = sorted(d_spin["threads"].unique())
    if not hosts or not procs:
        die("Fig1: missing either hosts sweep or spin process sweep")

    fig, ax1 = plt.subplots(figsize=FIG1_FIGSIZE)
    ax2 = ax1.twinx()

    fig1_text_size = BASE_TEXT_SIZE * FIG1_TEXT_SCALE
    fig1_tick_size = TICK_TEXT_SIZE * FIG1_TICK_SCALE
    fig1_legend_size = LEGEND_TEXT_SIZE * FIG1_LEGEND_SCALE
    fig1_marker_size = plt.rcParams["lines.markersize"] * FIG1_MARKER_SCALE

    nonspin = [x for x in d_hosts["lock_name"].unique() if x != "spin"]
    if not nonspin:
        die("Fig1: no non-spin locks in hosts sweep")

    for k in sort_lock_order(nonspin):
        dd = d_hosts[d_hosts.lock_name==k].groupby("hosts", as_index=False)["lock_p50_ns"].median().sort_values("hosts")
        st = ALG_STYLE.get(k, {"color": CB["black"], "ls": "-"})
        ax1.plot(
            dd["hosts"], dd["lock_p50_ns"],
            color=st["color"],
            linestyle="-",
            linewidth=FIG1_LINE_LW,
            marker=st.get("marker", "o"),
            markersize=fig1_marker_size,
            markerfacecolor="none",
            markeredgecolor=st["color"],
            markeredgewidth=FIG1_MARKER_EDGE_LW,
            label=f"{DISPLAY_NAMES.get(k,k)}",
        )

    dd = d_spin.groupby("threads", as_index=False)["lock_p50_ns"].median().sort_values("threads")
    st = ALG_STYLE["spin"]
    ax2.plot(
        dd["threads"], dd["lock_p50_ns"],
        color=GRAY_808080,
        linestyle=(0, (3, 2)),
        linewidth=FIG1_SPIN_LINE_LW,
        marker=st.get("marker", "o"),
        markersize=fig1_marker_size,
        markerfacecolor="none",
        markeredgecolor=GRAY_808080,
        markeredgewidth=FIG1_SPIN_MARKER_EDGE_LW,
        label="Spin",
    )

    ax1.set_xlabel("number of hosts", fontsize=fig1_text_size)
    ax1.set_ylabel("Distributed locks: p50 (ns)", fontsize=fig1_text_size)
    ax2.set_ylabel("Spin: p50 (ns)", fontsize=fig1_text_size)
    # no title (paper-friendly)
    ax1.set_xticks(sorted(set(hosts) | set(procs)))
    ax1.tick_params(axis="both", labelsize=fig1_tick_size)
    ax2.tick_params(axis="both", labelsize=fig1_tick_size)
    ax1.grid(True, which="both", axis="y", linestyle=":", alpha=0.25)
    ax1.set_ylim(bottom=0)
    ax2.set_ylim(bottom=0)
    ax1.spines["left"].set_linestyle(":"); ax1.spines["left"].set_alpha(0.8)
    ax2.spines["right"].set_linestyle(":"); ax2.spines["right"].set_alpha(0.8)

    h1,l1 = ax1.get_legend_handles_labels()
    h2,l2 = ax2.get_legend_handles_labels()
    labels = l1 + l2
    ax1.legend(
        h1 + h2,
        labels,
        ncols=legend_ncols_for_rows(len(labels), FIG1_LEGEND_ROWS),
        fontsize=fig1_legend_size,
        loc="upper center",
        bbox_to_anchor=(FIG1_LEGEND_X, FIG1_LEGEND_Y),
        frameon=False,
        columnspacing=1.0,
        handletextpad=0.6,
    )
    fig1_tag = (
        "cxl_hold0_gap0_locks1_ch1"
        f"_distHosts{fmt_range(hosts)}_distThreads1"
        f"_spinHosts1_spinProcs{fmt_range(procs)}"
    )
    savefig(fig, outdir, f"1_latency")

    # ---------- FIG 2: per-lock p50 bars + p50/p95 marks (shared broken y across locks) ----------
    d2 = df[(df.memory=="cxl") & (df.hold_ns==0) & (df.gap_ns==0) & (df.threads==1) & (df.channels==1) & (df.locks.isin([1,2,4,8]))]
    locks_for_fig2 = ["lfm","peterson","bakery","centralized"]

    p50_max = float(d2[d2.lock_name.isin(locks_for_fig2)]["lock_p50_ns"].max())
    p95_max = float(d2[d2.lock_name.isin(locks_for_fig2)]["lock_p95_ns"].max())
    p95_min = float(d2[d2.lock_name.isin(locks_for_fig2)]["lock_p95_ns"].min())

    p95_ylim = (max(1.0, p95_min * 0.9), p95_max * 1.25)
    # Use a single shared break position so all 4 figures are comparable.
    # Heuristic: break just above the non-outlier locks' p95 (so lfm's tail shows in top panel).
    p95_nonlfm = float(d2[(d2.lock_name.isin(locks_for_fig2)) & (d2.lock_name != "lfm")]["lock_p95_ns"].max())
    y_break = max(p50_max * 1.35, p95_nonlfm * 1.25)
    y_break = min(y_break, p95_max * 0.90)
    y_top_min = y_break * 1.2

    hosts = sorted(d2["hosts"].unique())
    x = np.arange(len(hosts))
    lock_counts = [1,2,4,8]
    width = 0.18

    for lock_alg in locks_for_fig2:
        fig, (ax_top, ax_bot) = plt.subplots(
            2, 1, sharex=True,
            figsize=DEFAULT_FIGSIZE,
            gridspec_kw={"height_ratios": [1.2, 2.4], "hspace": 0.05},
        )
        for j, lc in enumerate(lock_counts):
            dd = (
                d2[(d2.lock_name==lock_alg) & (d2.locks==lc)]
                .groupby("hosts", as_index=False)[["lock_p50_ns","lock_p95_ns"]]
                .median()
            )
            dd = pd.merge(pd.DataFrame({"hosts": hosts}), dd, on="hosts", how="left")
            if dd.isna().any().any():
                die(f"Fig2: missing data for {lock_alg}, locks={lc}")
            xpos = x + (j - 1.5) * width
            bst = FOUR_BAR_STYLE.get(lc, {"facecolor": "white", "hatch": ""})
            for ax in (ax_top, ax_bot):
                ax.bar(
                    xpos,
                    dd["lock_p50_ns"].to_numpy(),
                    width=width,
                    facecolor=bst["facecolor"],
                    hatch=bst["hatch"],
                    edgecolor="0.2",
                    linewidth=BAR_EDGE_LW,
                    alpha=0.85,
                    label="_nolegend_",
                    zorder=2,
                )
                # p95 as stock-style whiskers above each p50 bar (no connecting line)
                for cx, p50, p95 in zip(xpos, dd["lock_p50_ns"].to_numpy(), dd["lock_p95_ns"].to_numpy()):
                    ax.vlines(cx, p50, p95, colors="0.2", linewidth=WHISKER_LW, zorder=6)
                    cap = width * 0.30
                    ax.hlines(p95, cx - cap, cx + cap, colors="0.2", linewidth=CAP_LW, zorder=7)

        fig2_text_size = BASE_TEXT_SIZE * FIG2_TEXT_SCALE
        fig2_tick_size = TICK_TEXT_SIZE * FIG2_TEXT_SCALE
        fig2_legend_size = LEGEND_TEXT_SIZE * FIG2_TEXT_SCALE
        ax_bot.set_xlabel("Hosts", fontsize=fig2_text_size)
        ax_bot.set_ylabel("Latency p50/p95 (ns)", fontsize=fig2_text_size, labelpad=14)
        # no title (paper-friendly)

        ax_bot.set_xticks(x)
        ax_bot.set_xticklabels(hosts)
        ax_bot.yaxis.set_label_coords(-0.2, 0.65)
        ax_bot.tick_params(axis="both", labelsize=fig2_tick_size)
        ax_top.tick_params(axis="y", labelsize=fig2_tick_size)

        # shared broken y across all 4 lock figures
        ax_bot.set_ylim(0.0, y_break)
        ax_top.set_ylim(y_top_min, p95_max * 1.05)
        def fmt_k(val, _pos):
            if val >= 1000:
                if val % 1000 == 0:
                    return f"{int(val/1000)}k"
                return f"{val/1000:.1f}k"
            return f"{int(val)}"

        for ax in (ax_top, ax_bot):
            ax.ticklabel_format(axis="y", style="plain")
            ax.yaxis.set_major_formatter(FuncFormatter(fmt_k))
        # Simpler major ticks
        ax_bot.yaxis.set_major_locator(MultipleLocator(5000))

        for ax in (ax_top, ax_bot):
            ax.grid(True, which="both", axis="y", linestyle=":", alpha=0.25, zorder=0)

        # remove spines between axes + add break marks
        ax_top.spines["bottom"].set_visible(False)
        ax_bot.spines["top"].set_visible(False)
        ax_top.tick_params(labelbottom=False)
        d = .012
        kwargs = dict(transform=ax_top.transAxes, color="0.2", clip_on=False, linewidth=1.2)
        ax_top.plot((-d, +d), (-d, +d), **kwargs)
        ax_top.plot((1 - d, 1 + d), (-d, +d), **kwargs)
        kwargs.update(transform=ax_bot.transAxes)
        ax_bot.plot((-d, +d), (1 - d, 1 + d), **kwargs)
        ax_bot.plot((1 - d, 1 + d), (1 - d, 1 + d), **kwargs)

        # Legend: locks count only (p50 bars + p95 whiskers)
        handles = [
            Patch(
                facecolor=FOUR_BAR_STYLE[lc]["facecolor"],
                edgecolor="0.2",
                linewidth=BAR_EDGE_LW,
                hatch=FOUR_BAR_STYLE[lc]["hatch"],
                label=f"locks={lc}",
            )
            for lc in lock_counts
        ]
        ax_top.legend(
            handles=handles,
            ncols=2,
            fontsize=fig2_legend_size,
            loc="upper center",
            bbox_to_anchor=(0.5, 1.75),
            frameon=False,
            columnspacing=1.0,
            handletextpad=0.6,
        )

        fig2_tag = f"cxl_hold0_gap0_threads1_ch1_hosts{fmt_range(hosts)}_locks{fmt_range(lock_counts)}"
        savefig(fig, outdir, f"2_latency_{lock_alg}")

if __name__ == "__main__":
    main()
