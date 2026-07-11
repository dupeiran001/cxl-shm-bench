#!/usr/bin/env bash
# Regenerate the 6 bw_mem figures used in the paper (Figure 2 (a)-(f))
# from the CSVs in ../../result/bw_mem/raw/. Output PDFs and PNGs land in ../../result/bw_mem/plots/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RAW_DIR="${SCRIPT_DIR}/../../result/bw_mem/raw"
PLOTS_DIR="${SCRIPT_DIR}/../../result/bw_mem/plots"
PLOT="${SCRIPT_DIR}/plot_bw_mem.py"

mkdir -p "${PLOTS_DIR}"
cd "${RAW_DIR}"

plot_one() {
  local out_name="$1"; shift
  "${PLOT}" "$@" --label --out "${PLOTS_DIR}/${out_name}.png"
  "${PLOT}" "$@" --label --pdf --out "${PLOTS_DIR}/${out_name}.pdf"
  echo "[plot] ${out_name}"
}

plot_one dram                 dram                                 # Fig 2(a)
plot_one dax_local            dax_local                            # Fig 2(b)
plot_one dax_remote_1-4cxl    dax_remote  --overlay dax_1host_4cxl # Fig 2(c)
plot_one dax_2host_1cxl       dax_2host_1cxl                       # Fig 2(d)
plot_one dax_2host_4cxl       dax_2host_4cxl                       # Fig 2(e)
# Fig 2(f): current testbed maps 4 CXL modules (paper submission used 8).
# Plot whichever dataset is present.
if [[ -f dax_4host_4cxl_rd.csv ]]; then
  plot_one dax_4host_4cxl     dax_4host_4cxl                       # Fig 2(f), 4-module fabric
else
  plot_one dax_4host_8cxl     dax_4host_8cxl                       # Fig 2(f), original 8-module data
fi
