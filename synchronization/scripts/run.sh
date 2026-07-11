#!/usr/bin/env bash
# Run the two benchmark sweeps (shared CXL decentralized locks, and the
# shared CXL centralized coordinator) across the hosts listed in HOSTS.
# Results land in ../result/raw/ as run_all.csv and run_all_central.csv
# — the inputs consumed by plot_lock_figs.py.
#
# Required env:
#   HOSTS  - comma-separated SSH targets, e.g. "root@10.0.0.1,root@10.0.0.2,root@10.0.0.3,root@10.0.0.4"
#            All four hosts must be reachable via passwordless SSH and
#            must have the repo cloned at the same path, with lock_bench
#            already built (`make` in synchronization/).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_DIR="${SCRIPT_DIR}/../result/raw"
# BIN_ROOT is substituted for {BIN_ROOT} tokens in the JSON plans — it must
# point at the synchronization/ directory on the remote hosts (which is
# expected to hold the lock_bench binary at the same path on every host).
# Defaults to the local checkout's synchronization/ directory; override if
# your remote layout differs.
BIN_ROOT="${BIN_ROOT:-$(cd "${SCRIPT_DIR}/.." && pwd)}"

: "${HOSTS:?set HOSTS=root@hostA,root@hostB,root@hostC,root@hostD (4 hosts)}"

mkdir -p "$RAW_DIR"

run() {
  local plan="$1"; shift
  local out="$1";  shift
  echo "[run] plan=$(basename "$plan") out=$(basename "$out") bin_root=$BIN_ROOT"
  "$SCRIPT_DIR/run_commands.py" \
      --hosts "$HOSTS" \
      --plan  "$SCRIPT_DIR/$plan" \
      --bin_root "$BIN_ROOT" \
      --out_csv "$RAW_DIR/$out" \
      --direct "$@"
}

# shared CXL, decentralized locks (LFM/Peterson/Bakery/Spin)
run run_all.json            run_all.csv

# shared CXL, centralized lock server (needs non-init delay so the
# server comes up before clients connect)
run run_all_centralized.json run_all_central.csv --non_init_delay 1

echo "[run] done. Now: python3 $SCRIPT_DIR/plot_lock_figs.py"
