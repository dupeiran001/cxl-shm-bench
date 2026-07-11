#!/usr/bin/env bash
# Regenerate the 3 tool_memory_latency figures used in the paper
# (Figure 3 (a)-(c)) from the CSVs in ../../result/tool_memory_latency/raw/. Output PDFs and PNGs
# land in ../../result/tool_memory_latency/plots/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_DIR="${SCRIPT_DIR}/../../result/tool_memory_latency/raw"
PLOTS_DIR="${SCRIPT_DIR}/../../result/tool_memory_latency/plots"
PLOT="${SCRIPT_DIR}/plot_tool_memory_latency.py"

mkdir -p "${PLOTS_DIR}"

"${PLOT}" --input_dir "${RAW_DIR}" --out_dir "${PLOTS_DIR}" --label
"${PLOT}" --input_dir "${RAW_DIR}" --out_dir "${PLOTS_DIR}" --label --pdf
