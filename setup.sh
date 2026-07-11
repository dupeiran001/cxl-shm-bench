#!/usr/bin/env bash
# setup.sh — one-time reproducer setup for the multi-host CXL testbed.
#
# Run this ONCE. It:
#   1. brings the CXL fabric + g1..g4 to a known-good state via
#      scripts/cxl_remap.sh remap (points PXE at the production kernel, reboots,
#      maps the shared pool, runs prepare.sh — deps, SSH keys, CPU governor,
#      devdax mode, BF3 LINK_TYPE);
#   2. builds every artifact on this controller (incl. the cross-built A1 kernel
#      module and the upstream GAPBS reference); and
#   3. installs SSH keys and deploys the built tree to /tmp/cxl_shm_profiling on
#      every host.
#
# After it finishes, run each artifact's reproduce.sh — they all expect the repo
# built at /tmp/cxl_shm_profiling on the hosts.
#
# Usage:  ./setup.sh                  # full run: remap (power-cycles g1..g4) + build + deploy
#         SKIP_REMAP=1 ./setup.sh     # skip the remap; just build + deploy (testbed already up)
#         ./setup.sh --max-retries N  # any extra flags are forwarded to cxl_remap remap
# Env:    HOSTS="g1 g2 g3 g4"  REMOTE_ROOT=/tmp/cxl_shm_profiling
#         KDIR=<kernel build tree>  HOST_PASS=<first-boot root pw>
#         SKIP_REMAP=1     skip step 1 (fabric already mapped + hosts prepared)
#         REMAP_GRACE=<s>  grace before the destructive remap (default 5; 0 = none)
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REMOTE_ROOT="${REMOTE_ROOT:-/tmp/cxl_shm_profiling}"
read -r -a HOSTS <<< "${HOSTS:-g1 g2 g3 g4}"
HOST_PASS="${HOST_PASS:-123456}"
KDIR="${KDIR:-$REPO_ROOT/linux-pxe/linux}"        # kernel tree for cross-building A1

log(){ printf '[setup %s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die(){ log "FATAL: $*"; exit 1; }

# ---- 1. fabric + hosts -------------------------------------------------------
if [[ "${SKIP_REMAP:-0}" != 1 ]]; then
  log "step 1/4: cxl_remap.sh remap (PXE->prod kernel, reboot, map pool, prepare.sh)"
  log "  NOTE: this POWERS OFF ${HOSTS[*]} and reboots the H3."
  log "        Ctrl-C now to abort, or use SKIP_REMAP=1 if the testbed is already up."
  sleep "${REMAP_GRACE:-5}"
  # Running setup.sh IS the opt-in, so auto-confirm the remap (--yes); any extra
  # flags the caller passed (e.g. --max-retries N) are forwarded after it.
  "$REPO_ROOT/scripts/cxl_remap.sh" remap --yes "$@" \
    || die "cxl_remap failed — see its log, fix, and re-run (or SKIP_REMAP=1 if already up)"
else
  log "step 1/4: SKIP_REMAP=1 — assuming the fabric is already mapped + hosts prepared"
fi

# ---- 2. build everything on this controller ---------------------------------
log "step 2/4: build all artifacts on this controller"
if [[ -d "$KDIR" ]]; then
  make -C "$KDIR" M="$REPO_ROOT/interleave_dax" modules >/dev/null 2>&1 \
    && log "  A1 interleave_dax.ko built" \
    || log "  WARNING: kernel-module build failed — A1/insmod may fail (build it by hand)"
else
  log "  no kernel tree at $KDIR — skipping A1 module (set KDIR= to enable)"
fi
make -C "$REPO_ROOT" all || die "make all failed (A2 lib+verify, A3, A4, A5, A7)"
make -C "$REPO_ROOT/gapbs" >/dev/null 2>&1 \
  && log "  upstream GAPBS reference built" \
  || log "  WARNING: upstream gapbs build failed (A5 needs gapbs/bfs etc.)"

# ---- 3. keys + deploy to every host -----------------------------------------
log "step 3/4: install keys + deploy tree -> $REMOTE_ROOT on ${HOSTS[*]}"
for h in "${HOSTS[@]}"; do
  sshpass -p "$HOST_PASS" ssh-copy-id -o StrictHostKeyChecking=no "root@$h.dc" >/dev/null 2>&1 || true
  ssh -4 -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 "root@$h.dc" true 2>/dev/null \
    || die "$h.dc unreachable over SSH (is it booted? keys installed?)"
  rsync -a --delete \
    --exclude '.git' --exclude 'linux-pxe' --exclude 'parsec/.cache' \
    ${RSYNC_EXCLUDES:-} \
    "$REPO_ROOT/" "root@$h.dc:$REMOTE_ROOT/" || die "$h: rsync failed"
  log "  $h: deployed"
done

# ---- 4. sanity: the reproduce scripts' key binaries are present -------------
log "step 4/4: verify built binaries on ${HOSTS[*]}"
miss=0
for h in "${HOSTS[@]}"; do
  ssh -4 -o BatchMode=yes "root@$h.dc" "
      for b in global_allocator/verify/verify_shm benchmark/bw_mem/bw_mem \
               synchronization/lock_bench gapbs_mh/bfs_mh hashtable_mh/ht_bench; do
        [ -x $REMOTE_ROOT/\$b ] || { echo MISSING \$b; }
      done" 2>/dev/null | grep -q MISSING \
    && { log "  $h: WARNING — some binaries missing (see: ssh root@$h.dc 'ls $REMOTE_ROOT/*/')"; miss=$((miss+1)); } \
    || log "  $h: all key binaries present"
done

echo
if (( miss == 0 )); then
  log "DONE — testbed ready. Reproduce any artifact, e.g.:"
  log "    ./global_allocator/reproduce.sh      # A2"
  log "    RUNS=3 ./benchmark/reproduce.sh all  # A3 (bw + lat + rtt + rdma)"
else
  die "$miss host(s) missing binaries — fix the build/deploy above and re-run"
fi
