#!/usr/bin/env bash
# Start the RTT benchmark as the server (reader-first) on host 1.
# Run this BEFORE client.sh on the other host.
#
# Overridable via env vars:
#   ITER    (default 1000000)            - iterations
#   FLUSH   (default clflushopt; "clwb") - flush primitive
#   OFFSET  (default 0)                  - byte offset into the DAX mapping (4K aligned)
#   DAX_DEV (default /dev/interleave_dax) - DAX device path
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$(cd "$SCRIPT_DIR/../../rtt" && pwd)/rtt"
ITER="${ITER:-1000000}"
FLUSH="${FLUSH:-clflushopt}"
OFFSET="${OFFSET:-0}"
DAX_DEV="${DAX_DEV:-/dev/interleave_dax}"

[ -x "$BIN" ] || { echo "[server] $BIN not built. Run 'make' in benchmark/rtt/." >&2; exit 1; }

args=(--mode=server --iter="$ITER" --offset="$OFFSET" --dax_dev="$DAX_DEV")
if [ "$FLUSH" = "clwb" ]; then
  args+=(--clwb)
fi

echo "[server] $BIN ${args[*]}"
exec "$BIN" "${args[@]}"
