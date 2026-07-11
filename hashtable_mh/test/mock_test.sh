#!/usr/bin/env bash
# mock_test.sh — LOCAL validation of hashtable_mh (no CXL hardware needed).
#
# Runs ht_bench against a tmpfs-backed fake DAX device (a file in /dev/shm),
# first as a single-"host" smoke test, then as a 2-"host" functional test
# with two processes sharing the same mapping. Asserts clean exits and
# "error_count: 0" in every output. This is the required local gate before
# touching the testbed with reproduce.sh.
#
# Usage: hashtable_mh/test/mock_test.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
DEV="${DEV:-/dev/shm/ht_mock_$$.bin}"
OUT="$(mktemp -d)"
PASS=1

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
fail() { log "FAIL: $*"; PASS=0; }
cleanup() {
  pkill -f "ht_bench.*$DEV" 2>/dev/null
  rm -f "$DEV"
  rm -rf "$OUT"
}
trap cleanup EXIT

log "building ht_bench (builds ../global_allocator lib if missing)"
make -C "$ROOT" >/dev/null || { log "FATAL: build failed"; exit 1; }

# Small table so init/preload stay fast on a dev box.
COMMON="--buckets=4096 --keys=20000 --slab-mb=32 --shm-mb=512 \
        --seconds=2 --threads=4 --mix=50 --barrier-timeout=60"

check_log() { # $1=logfile $2=label
  grep -q '^error_count: 0$' "$1" || fail "$2: error_count nonzero or missing"
  grep -q '^mops: ' "$1" || fail "$2: no mops line"
}

# ---------------- single-process smoke (hosts=1, same code path) -----------
rm -f "$DEV"; truncate -s 2G "$DEV"
log "smoke test: 1 host, 4 threads, mix 50"
timeout 120 "$ROOT/ht_bench" --dev="$DEV" --hosts=1 --host_id=0 --init \
  $COMMON >"$OUT/smoke.log" 2>&1
rc=$?
[[ $rc -eq 0 ]] || fail "smoke: exit rc=$rc"
check_log "$OUT/smoke.log" "smoke"
grep -q '^AGGREGATE ' "$OUT/smoke.log" || fail "smoke: no AGGREGATE line"

# ---------------- two "hosts" as two local processes -----------------------
rm -f "$DEV"; truncate -s 2G "$DEV"
log "2-host functional test: two processes on one mapping"
timeout 180 "$ROOT/ht_bench" --dev="$DEV" --hosts=2 --host_id=0 --init \
  $COMMON >"$OUT/h0.log" 2>&1 &
P0=$!
sleep 2   # let host 0 reset the allocator before host 1 maps it
timeout 180 "$ROOT/ht_bench" --dev="$DEV" --hosts=2 --host_id=1 \
  $COMMON >"$OUT/h1.log" 2>&1 &
P1=$!
wait "$P0"; rc0=$?
wait "$P1"; rc1=$?
[[ $rc0 -eq 0 ]] || fail "host0: exit rc=$rc0"
[[ $rc1 -eq 0 ]] || fail "host1: exit rc=$rc1"
for i in 0 1; do
  grep -q "^HOST $i RESULTS$" "$OUT/h$i.log" || fail "host$i: no RESULTS block"
  check_log "$OUT/h$i.log" "host$i"
done
grep -q '^AGGREGATE ' "$OUT/h0.log" || fail "host0: no AGGREGATE line"
grep -q 'errors_all=0' "$OUT/h0.log" || fail "aggregate errors_all nonzero"

if [[ $PASS -eq 1 ]]; then
  log "PASS"
  log "smoke:  $(grep '^AGGREGATE ' "$OUT/smoke.log")"
  log "host0:  $(grep '^mops: ' "$OUT/h0.log")"
  log "host1:  $(grep '^mops: ' "$OUT/h1.log")"
  log "2-host: $(grep '^AGGREGATE ' "$OUT/h0.log")"
  exit 0
fi

log "mock test FAILED — dumping logs"
for f in smoke h0 h1; do
  [[ -f "$OUT/$f.log" ]] && { echo "----- $f -----"; cat "$OUT/$f.log"; }
done
exit 1
