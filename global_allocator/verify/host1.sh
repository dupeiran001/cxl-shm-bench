#!/usr/bin/env bash
# Run the cross-host verifier as the FIRST host (host_id=0).
# This host creates the shared barrier and data objects; host2 attaches.
#
# Overridable via env vars:
#   ROUNDS   (default 100)            - verification rounds
#   DAX_DEV  (default /dev/interleave_dax) - shared CXL device path
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/verify_shm"
ROUNDS="${ROUNDS:-100}"
DAX_DEV="${DAX_DEV:-/dev/interleave_dax}"

[ -x "$BIN" ] || { echo "[host1] $BIN not found. Run 'make verify' from global_allocator/." >&2; exit 1; }
[ -e "$DAX_DEV" ] || { echo "[host1] $DAX_DEV not found. Is interleave_dax loaded?" >&2; exit 1; }

echo "[host1] host_id=0 rounds=$ROUNDS dev=$DAX_DEV"
exec "$BIN" 0 "$ROUNDS" "$DAX_DEV"
