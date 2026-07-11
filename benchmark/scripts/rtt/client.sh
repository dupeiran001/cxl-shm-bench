#!/usr/bin/env bash
# Start the RTT benchmark as the client (writer-first) on host 2.
# The server must already be running on host 1 before this starts.
#
# Overridable via env vars:
#   ITER    (default 1000000)            - iterations (must match server's)
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

[ -x "$BIN" ] || { echo "[client] $BIN not built. Run 'make' in benchmark/rtt/." >&2; exit 1; }

args=(--mode=client --iter="$ITER" --offset="$OFFSET" --dax_dev="$DAX_DEV")
if [ "$FLUSH" = "clwb" ]; then
  args+=(--clwb)
fi

echo "[client] $BIN ${args[*]}"
exec "$BIN" "${args[@]}"
