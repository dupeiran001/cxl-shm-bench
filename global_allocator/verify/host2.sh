#!/usr/bin/env bash
# Run the cross-host verifier as the SECOND host (host_id=1).
# host1 must be started first (or concurrently); this host attaches to the
# shared objects host1 created.
#
# Overridable via env vars:
#   ROUNDS   (default 100)            - verification rounds
#   DAX_DEV  (default /dev/interleave_dax) - shared CXL device path
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$SCRIPT_DIR/verify_shm"
ROUNDS="${ROUNDS:-100}"
DAX_DEV="${DAX_DEV:-/dev/interleave_dax}"

[ -x "$BIN" ] || { echo "[host2] $BIN not found. Run 'make verify' from global_allocator/." >&2; exit 1; }
[ -e "$DAX_DEV" ] || { echo "[host2] $DAX_DEV not found. Is interleave_dax loaded?" >&2; exit 1; }

echo "[host2] host_id=1 rounds=$ROUNDS dev=$DAX_DEV"
exec "$BIN" 1 "$ROUNDS" "$DAX_DEV"
