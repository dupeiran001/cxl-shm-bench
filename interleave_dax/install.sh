#!/usr/bin/env bash
set -euo pipefail

# Directory of this script (module dir)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Your physical ranges:
#   0x3040000000 - 0x5040000000  (128 GiB)
#   0x5040000000 - 0x7040000000  (128 GiB)
#
# Treat this as one region:
#   base_phys = 0x3040000000
#   cfg      = "0-128:1,128-256:1"
#
# meaning:
#   [base + 0 GiB  .. base + 128 GiB)  weight 1
#   [base + 128 GiB.. base + 256 GiB)  weight 1


# EDIT THESE to match your platform's SRAT output (see: `dmesg | grep -i SRAT`).
BASE_PHYS="0x4080000000"
CONFIG_STR="0-128:1,128-256:1,256-384:1,384-512:1,512-640:1,640-768:1,768-896:1,896-1024:1"

echo "[*] Inserting interleave_dax.ko"
echo "    base_phys=${BASE_PHYS}"
echo "    config=${CONFIG_STR}"

insmod "$SCRIPT_DIR/interleave_dax.ko" \
    base_phys="$BASE_PHYS" \
    config="$CONFIG_STR"

echo "[*] Module loaded. Devices:"
ls -l /dev/interleave_dax* || echo "  (no devices found?)"

echo "[*] Recent dmesg:"
dmesg | tail -n 20

