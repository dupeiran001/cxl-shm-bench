# gapbs_mh/scripts

Helper scripts for Artifact A5 (multi-host GAPBS). The artifact overview, build,
and the Figure 7 reproduction runbook are in [`../README.md`](../README.md).

- **`plot_gapbs.py`** — regenerates Figure 7 (a)–(d) from
  `../result/raw/gapbs.csv` into `../result/plots/`. `python3 plot_gapbs.py`
  (arg-less) uses those defaults; `--bench <KERNEL>` plots a single panel,
  `--csv` / `--out_dir` override the input/output paths.
- **`sync_profile.py`** — Table VI (SYNC_STATS) analysis: parses the
  `SYNC_STATS` counter logs and writes `../result/raw/TABLE_VI_QUANT.md`.
  Invoked by `../profile_sync.sh`; not run by hand.
