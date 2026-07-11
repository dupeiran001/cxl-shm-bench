#!/usr/bin/env bash
# reproduce.sh — one-script reproduction of artifact A5 (gapbs_mh/):
#   Fig 7 (a)-(d): BFS / PR / PR-SPMV / TC across local DRAM, remote CXL,
#   2-host shared CXL, and 4-host shared CXL.
#
# Replaces the manual workflow (hand-started binaries on each host) with an
# SSH-orchestrated launcher; the kernels still rendezvous via the shared-memory
# barrier internally.
#
# Config matrix (paper parameters, from result/raw/gapbs.csv):
#   BFS     g27 k50 n10  shm 64000   PR    g25 k50 n3 (shared: n1 --sync-k 1)
#   PR-SPMV g25 k50 n3   shm 64000   TC    g20 k50 n3 shm 16384
#
# Usage:  gapbs_mh/reproduce.sh [all|dram|remote|shared2|shared4]
#         RUNS=3 QUICK=0 gapbs_mh/reproduce.sh all
set -uo pipefail

RUNS="${RUNS:-3}"
QUICK="${QUICK:-0}"
REMOTE_ROOT=/tmp/cxl_shm_profiling
ILDAX_BASE=0x4080000000
ILDAX_CFG_4="0-128:1,128-256:1,256-384:1,384-512:1"
H_SINGLE=g2                          # dram + remote_cxl host
HOSTS4=(g1 g2 g3 g4)
THREADS_SINGLE=(8 16 32 64)          # OMP threads, single-host configs
THREADS_PER_HOST=(4 8 16 32)         # per-host, shared configs

KERNELS=(${KERNELS[@]:-BFS PR PR-SPMV TC})
declare -A BIN=([BFS]=bfs [PR]=pr [PR-SPMV]=pr_spmv [TC]=tc)
declare -A G=([BFS]=27 [PR]=25 [PR-SPMV]=25 [TC]=20)
declare -A K=([BFS]=50 [PR]=50 [PR-SPMV]=50 [TC]=50)
declare -A N=([BFS]=10 [PR]=3 [PR-SPMV]=3 [TC]=3)
declare -A N_SHARED=([BFS]=10 [PR]=1 [PR-SPMV]=3 [TC]=3)
declare -A XTRA_SHARED=([BFS]="" [PR]="--sync-k 1" [PR-SPMV]="" [TC]="")
declare -A SHM=([BFS]=64000 [PR]=64000 [PR-SPMV]=64000 [TC]=16384)

if [[ "$QUICK" == 1 ]]; then
  THREADS_SINGLE=(8); THREADS_PER_HOST=(4)
  for k in "${KERNELS[@]}"; do G[$k]=16; N[$k]=1; N_SHARED[$k]=1; SHM[$k]=2048; done
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CSV="$REPO_ROOT/gapbs_mh/result/raw/gapbs.csv"
# QUICK smokes use tiny graphs — never mix them into the production CSV.
[[ "$QUICK" == 1 ]] && CSV="$REPO_ROOT/gapbs_mh/result/raw/gapbs_quick.csv"
HDR="benchmark,config,total_threads,threads_per_host,g,k,trials,avg_s,stages,cmd_host0,cmd_host1"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "FATAL: $*"; exit 1; }
gssh() { local h=$1; shift; ssh -4 -n -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "root@$h.dc" "$@"; }

csv_row() { echo "$1" >> "$CSV"; }

preflight() {
  for h in "${HOSTS4[@]}"; do
    gssh "$h" "[ -x $REMOTE_ROOT/gapbs_mh/bfs_mh ] && [ -x $REMOTE_ROOT/gapbs/bfs ] || exit 1" \
      || die "$h: gapbs binaries missing (make -C gapbs_mh; upstream gapbs built?)"
  done
  local bak="$REPO_ROOT/gapbs_mh/result/raw/paper_baseline"
  if [[ ! -d "$bak" ]]; then
    mkdir -p "$bak"; cp "$CSV" "$bak/" 2>/dev/null || true
    log "previous gapbs.csv preserved in $bak/"
  fi
  [[ -f "$CSV" ]] || echo "$HDR" > "$CSV"
}

# window mode flips on one host (remote_cxl needs system-ram node; shared needs devdax)
window_system_ram() {
  gssh "$1" "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; pkill -x lock_bench 2>/dev/null; pkill -x ht_bench 2>/dev/null; pkill -x bw_mem 2>/dev/null; pkill -f '[g]apbs_mh/' 2>/dev/null; sleep 0.5; rmmod interleave_dax 2>/dev/null; daxctl reconfigure-device -m system-ram dax0.0 >/dev/null 2>&1; daxctl online-memory dax0.0 >/dev/null 2>&1; numactl -H | grep -q 'node 1' " \
    || die "$1: failed to online window as system-ram"
}
window_devdax() {
  gssh "$1" "daxctl offline-memory dax0.0 >/dev/null 2>&1; daxctl reconfigure-device -f -m devdax dax0.0 >/dev/null 2>&1; [ -e /dev/dax0.0 ]" \
    || die "$1: failed to return window to devdax"
}
ildax_all() {
  for h in "${HOSTS4[@]}"; do
    gssh "$h" "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; pkill -x lock_bench 2>/dev/null; pkill -x ht_bench 2>/dev/null; pkill -x bw_mem 2>/dev/null; pkill -f '[g]apbs_mh/' 2>/dev/null; sleep 0.5; rmmod interleave_dax 2>/dev/null; insmod $REMOTE_ROOT/interleave_dax/interleave_dax.ko base_phys=$ILDAX_BASE config=$ILDAX_CFG_4 && [ -e /dev/interleave_dax ]" \
      || die "$h: interleave_dax load failed"
  done
}

parse_avg() { awk '/Average Time:/{v=$3} END{print v}'; }

run_single() {  # $1=config(dram|remote_cxl)
  local cfg=$1 pre=""
  [[ "$cfg" == remote_cxl ]] && { window_system_ram "$H_SINGLE"; pre="numactl --membind=1 "; }
  for kern in "${KERNELS[@]}"; do
    for t in "${THREADS_SINGLE[@]}"; do
      local cmd="OMP_NUM_THREADS=$t ${pre}./gapbs/${BIN[$kern]} -g ${G[$kern]} -k ${K[$kern]} -n ${N[$kern]} -l"
      log "[$cfg] $kern t=$t"
      local avg
      avg=$(gssh "$H_SINGLE" "cd $REMOTE_ROOT && timeout 3600 env $cmd 2>/dev/null" | parse_avg)
      [[ -n "$avg" ]] || die "[$cfg] $kern t=$t: no Average Time parsed"
      csv_row "$kern,$cfg,$t,$t,${G[$kern]},${K[$kern]},${N[$kern]},$avg,${N[$kern]},\"$cmd\","
    done
  done
  [[ "$cfg" == remote_cxl ]] && window_devdax "$H_SINGLE"
}

run_shared() {  # $1=nhosts(2|4)
  local nh=$1
  ildax_all
  # Wipe shared metadata: stale barrier/segment state from killed runs
  # deadlocks the next rendezvous (see global_allocator/reproduce.sh note).
  gssh g1 "timeout 120 $REMOTE_ROOT/benchmark/bw_mem/bw_mem --dax_dev /dev/interleave_dax --dax_offset 0 --size 256M --parallel 1 --repetition 1 --test bzero >/dev/null 2>&1" || log "WARN: metadata wipe failed"
  local cfg="shared_${nh}hosts"
  for kern in "${KERNELS[@]}"; do
    for t in "${THREADS_PER_HOST[@]}"; do
      local total=$(( t * nh )) pids=() host cmd0=""
      log "[$cfg] $kern t/host=$t (total $total)"
      # Fresh metadata for EVERY multi-host launch
      for hh in "${HOSTS4[@]:0:$nh}"; do gssh "$hh" "for b in bfs_mh pr_mh pr_spmv_mh tc_mh; do pkill -9 -x \$b 2>/dev/null; done; true"; done
      gssh g1 "timeout 120 $REMOTE_ROOT/benchmark/bw_mem/bw_mem --dax_dev /dev/interleave_dax --dax_offset 0 --size 256M --parallel 1 --repetition 1 --test bzero >/dev/null 2>&1" || log "WARN: wipe failed"
      # Launch every rank as a detached transient unit, then poll
      # host logs for completion with a hard deadline.
      local tag="gapbs$$_${RANDOM}" host cmd0=""
      for i in $(seq 0 $((nh-1))); do
        host=${HOSTS4[$i]}
        local cmd="OMP_NUM_THREADS=$t ./gapbs_mh/${BIN[$kern]}_mh -g ${G[$kern]} -k ${K[$kern]} --host_id=$i --hosts=$nh --dev=/dev/interleave_dax --shm-mb=${SHM[$kern]} -n ${N_SHARED[$kern]} ${XTRA_SHARED[$kern]} -l"
        [[ $i == 0 ]] && cmd0="$cmd"
        gssh "$host" "systemd-run --collect --unit=ae-${tag}-h${i} bash -c 'cd $REMOTE_ROOT && env $cmd >/tmp/${tag}_h${i}.log 2>&1'" >/dev/null \
          || die "[$cfg] $kern t=$t: launch failed on $host"
        if [[ $i == 0 ]]; then
          # Deterministic rendezvous: rank 1..n attach segments rank 0 creates,
          # and the attach path does NOT retry — launching on a timer is a
          # race (observed: PR rank1 died with shm_attach_id ENOENT). Wait for
          # rank 0 to reach its initial barrier before launching the rest.
          local init_deadline=$(( $(date +%s) + 600 ))
          until gssh "${HOSTS4[0]}" "grep -q 'waiting at initial barrier' /tmp/${tag}_h0.log 2>/dev/null"; do
            (( $(date +%s) < init_deadline )) || die "[$cfg] $kern t=$t: rank0 never reached the initial barrier"
            sleep 2
          done
        fi
      done
      # Poll: only rank 0 prints "Average Time" (non-initiator ranks finish
      # silently after their trials), so completion = rank 0 reports + a short
      # grace for the other units to exit.
      local deadline=$(( $(date +%s) + 5400 )) all_done=0
      while (( $(date +%s) < deadline )); do
        if gssh "${HOSTS4[0]}" "grep -q 'Average Time' /tmp/${tag}_h0.log 2>/dev/null"; then
          all_done=1; break
        fi
        sleep 5
      done
      if (( all_done )); then
        local grace=$(( $(date +%s) + 30 ))
        while (( $(date +%s) < grace )); do
          local busy=0
          for i in $(seq 1 $((nh-1))); do
            gssh "${HOSTS4[$i]}" "systemctl is-active --quiet ae-${tag}-h${i} 2>/dev/null" && { busy=1; break; }
          done
          (( busy )) || break
          sleep 3
        done
      fi
      if (( ! all_done )); then
        for i in $(seq 0 $((nh-1))); do gssh "${HOSTS4[$i]}" "systemctl stop ae-${tag}-h${i} 2>/dev/null; true"; done
        die "[$cfg] $kern t=$t: deadline (ranks incomplete)"
      fi
      local avg; avg=$(gssh "${HOSTS4[0]}" "cat /tmp/${tag}_h0.log" | parse_avg)
      for i in $(seq 0 $((nh-1))); do gssh "${HOSTS4[$i]}" "rm -f /tmp/${tag}_h${i}.log"; done
      [[ -n "$avg" ]] || die "[$cfg] $kern t=$t: no Average Time parsed"
      csv_row "$kern,$cfg,$total,$t,${G[$kern]},${K[$kern]},${N_SHARED[$kern]},$avg,${N_SHARED[$kern]},\"$cmd0\",\"${cmd0/host_id=0/host_id=1}\""
    done
  done
}

run_plots() {
  (cd "$REPO_ROOT/gapbs_mh/scripts" && python3 plot_gapbs.py) || log "WARN: plot failed"
}

main() {
  preflight
  for run in $(seq 1 "$RUNS"); do
    log "===== GAPBS pass $run/$RUNS ====="
    case "${1:-all}" in
      dram)    run_single dram ;;
      remote)  run_single remote_cxl ;;
      shared2) run_shared 2 ;;
      shared4) run_shared 4 ;;
      all)     run_single dram; run_single remote_cxl; run_shared 2; run_shared 4 ;;
      *) die "usage: $0 [all|dram|remote|shared2|shared4]" ;;
    esac
  done
  run_plots
  log "done -> $CSV"
}
main "${1:-all}"
