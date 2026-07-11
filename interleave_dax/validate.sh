#!/usr/bin/env bash
# Validate that /dev/interleave_dax is serving reads at an interleaved
# bandwidth. Requires fio (ioengine=mmap) since the module only
# implements .mmap, not .read/.write.
#
# Overridable via env vars:
#   SIZE  (default 8G)   - per-job working set; stay within total backing
#   BS    (default 2m)   - block size
#   JOBS  (default 8)    - number of fio jobs (concurrency)
#   RW    (default read) - fio rw mode (read|write|randread|randwrite)
set -euo pipefail

DEV=/dev/interleave_dax
SIZE="${SIZE:-8G}"
BS="${BS:-2m}"
JOBS="${JOBS:-8}"
RW="${RW:-read}"

die() { echo "[validate] ERROR: $*" >&2; exit 1; }

command -v fio >/dev/null 2>&1 \
  || die "fio not found. Install it (e.g. 'apt install fio' or 'dnf install fio')."

lsmod | awk '{print $1}' | grep -qx interleave_dax \
  || die "kernel module 'interleave_dax' is not loaded. Run ./install.sh first."

[ -c "$DEV" ] \
  || die "$DEV not found or not a character device. Check dmesg for module init errors."

echo "[validate] fio: $(fio --version)"
echo "[validate] device: $DEV"
echo "[validate] params: rw=$RW bs=$BS size=$SIZE numjobs=$JOBS"
echo

fio --name=il_bw \
    --filename="$DEV" \
    --ioengine=mmap \
    --rw="$RW" \
    --bs="$BS" \
    --size="$SIZE" \
    --numjobs="$JOBS" \
    --group_reporting


