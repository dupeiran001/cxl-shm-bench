#!/usr/bin/env bash
# reproduce.sh — one-script reproduction of artifact A6 (parsec/):
#   Table V — intrinsic synchronization profile (atomics / locks / critical
#   sections / barriers within the ROI) of the PARSEC + SPLASH benchmarks on
#   a single coherent host. No CXL hardware involved.
#
# Deploys the instrumented PARSEC tree to the target host's tmpfs, fetches
# inputs (cached locally after the first fetch), builds with the gcc-hooks
# config, runs the sweep RUNS times, pulls each run's logs back into
# results/run_<timestamp>/, and aggregates mean±std into
# results/summaries/TABLE_V_AGG.md.
#
# Usage:
#   parsec/reproduce.sh                    # Table V subset, RUNS=3
#   FULL=1 parsec/reproduce.sh             # all 41 benchmarks per run
#   RUNS=1 HOST=g3 parsec/reproduce.sh
set -uo pipefail

RUNS="${RUNS:-3}"
HOST="${HOST:-g4}"
FULL="${FULL:-0}"
THREADS="${THREADS:-16}"
REMOTE_ROOT=/tmp/cxl_shm_profiling

# Table V benchmark subset (paper order); FULL=1 runs everything.
TABLE_V="parsec.canneal parsec.dedup parsec.fluidanimate parsec.streamcluster splash2x.lu_ncb splash2.radiosity splash2.raytrace"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULTS="$REPO_ROOT/parsec/results"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "FATAL: $*"; exit 1; }
gssh() { ssh -4 -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "root@$HOST.dc" "$@"; }

gssh true || die "$HOST unreachable"

log "deploying parsec tree to $HOST (first time is large)"
rsync -az -e "ssh -4 -o BatchMode=yes" --exclude=results \
  "$REPO_ROOT/parsec/" "root@$HOST.dc:$REMOTE_ROOT/parsec/" || die "rsync failed"

# The host's setup.sh downloads the input tarball from the archival Zenodo
# mirror (doi:10.5281/zenodo.21314344, SHA-256-verified) on the first setup of
# a boot; pre-place it in parsec/input-archive/ before running to skip the
# download (offline runs).

log "setup + gcc-hooks build (skipped if already built this boot; ~30 min cold)"
# "Already built" must mean: inputs unpacked AND the fussiest binaries exist
# (a partial build can leave early packages present while later ones failed).
gssh "cd $REMOTE_ROOT/parsec && [ -x pkgs/kernels/dedup/inst/*/bin/dedup ] && [ -x pkgs/kernels/canneal/inst/*/bin/canneal ] && [ -d pkgs/kernels/canneal/inputs ] && ls pkgs/kernels/canneal/inputs/*.tar >/dev/null 2>&1 && echo 'already built'" \
  | grep -q 'already built' || gssh "cd $REMOTE_ROOT/parsec && PARSEC_JOBS=32 timeout 3600 ./setup.sh" \
  || die "parsec setup/build failed"

only_env=""
[[ "$FULL" != 1 ]] && only_env="ONLY=\"$TABLE_V\""

for run in $(seq 1 "$RUNS"); do
  log "=== PARSEC sweep $run/$RUNS (THREADS=$THREADS $([ "$FULL" == 1 ] && echo full || echo 'Table V subset')) ==="
  gssh "cd $REMOTE_ROOT/parsec && CONFIG=gcc-hooks INPUT=simlarge THREADS=$THREADS $only_env timeout 7200 ./run_all_simlarge.sh" \
    | tail -3 || log "WARN: sweep $run reported failures (see SANITY in that run dir)"
  # pull the newest run directory back
  latest=$(gssh "ls -1d $REMOTE_ROOT/parsec/results/run_* 2>/dev/null | tail -1")
  [[ -n "$latest" ]] || die "no results directory produced on $HOST"
  rsync -az -e "ssh -4 -o BatchMode=yes" "root@$HOST.dc:$latest" "$RESULTS/" || die "result fetch failed"
  log "fetched $(basename "$latest")"
done

log "aggregating across runs"
python3 "$RESULTS/scripts/aggregate_runs.py" || die "aggregation failed"
log "done — results/summaries/TABLE_V_AGG.md"
