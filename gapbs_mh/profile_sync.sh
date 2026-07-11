#!/usr/bin/env bash
# profile_sync.sh — instrumented (SYNC_STATS) profiling runs feeding Table VI.
#
# gapbs_mh + global_allocator with SYNC_STATS=1 on the hosts, runs
# ONE instrumented trial per kernel for shared_2hosts and shared_4hosts
# (instrumentation perturbs timing, so these runs are separate from the
# reproduce.sh timing runs), collects per-host logs, produces
# result/raw/TABLE_VI_QUANT.md, then restores clean builds.
#
# Usage: gapbs_mh/profile_sync.sh   [QUICK=1]
set -uo pipefail

QUICK="${QUICK:-0}"
REMOTE_ROOT=/tmp/cxl_shm_profiling
ILDAX_BASE=0x4080000000
ILDAX_CFG_4="0-128:1,128-256:1,256-384:1,384-512:1"
HOSTS4=(g1 g2 g3 g4)

KERNELS=(bfs pr pr_spmv tc)
declare -A G=([bfs]=27 [pr]=25 [pr_spmv]=25 [tc]=20)
declare -A SHM=([bfs]=64000 [pr]=64000 [pr_spmv]=64000 [tc]=16384)
declare -A XTRA=([bfs]="" [pr]="--sync-k 1" [pr_spmv]="" [tc]="")
T_PER_HOST=16
if [[ "$QUICK" == 1 ]]; then
  for k in "${KERNELS[@]}"; do G[$k]=16; SHM[$k]=2048; done
  T_PER_HOST=4
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOGDIR="$REPO_ROOT/gapbs_mh/result/raw/sync_profile_logs"
LOGDIR_REMOTE=/tmp/ae_sync_profile_logs

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "FATAL: $*"; exit 1; }
gssh() { local h=$1; shift; ssh -4 -n -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "root@$h.dc" "$@"; }

mkdir -p "$LOGDIR"
for h in "${HOSTS4[@]}"; do gssh "$h" "mkdir -p $LOGDIR_REMOTE"; done

build_fleet() {  # $1 = "SYNC_STATS=1" or ""
  local flag="$1"
  for h in "${HOSTS4[@]}"; do
    gssh "$h" "cd $REMOTE_ROOT && make -C global_allocator clean >/dev/null 2>&1; make -C gapbs_mh clean >/dev/null 2>&1; timeout 600 make -C global_allocator lib $flag -j8 >/dev/null 2>&1 && timeout 600 make -C gapbs_mh $flag -j8 >/dev/null 2>&1" \
      || die "$h: build $flag failed"
  done
  log "fleet built ${flag:-clean}"
}

log "building instrumented binaries on the fleet"
build_fleet "SYNC_STATS=1"

log "loading interleave profile"
for h in "${HOSTS4[@]}"; do
  gssh "$h" "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; pkill -x lock_bench 2>/dev/null; pkill -x ht_bench 2>/dev/null; pkill -x bw_mem 2>/dev/null; pkill -f '[g]apbs_mh/' 2>/dev/null; sleep 0.5; rmmod interleave_dax 2>/dev/null; insmod $REMOTE_ROOT/interleave_dax/interleave_dax.ko base_phys=$ILDAX_BASE config=$ILDAX_CFG_4 && [ -e /dev/interleave_dax ]" \
    || die "$h: interleave_dax load failed"
done

run_profiled() {  # $1=nhosts
  local nh=$1
  local cfg="shared_${nh}hosts"
  for kern in "${KERNELS[@]}"; do
    log "[profile/$cfg] $kern (1 trial, t/host=$T_PER_HOST)"
    for i in $(seq 0 $((nh-1))); do
      gssh "${HOSTS4[$i]}" "for b in bfs_mh pr_mh pr_spmv_mh tc_mh; do pkill -9 -x \$b 2>/dev/null; done; true"
    done
    gssh g1 "timeout 120 $REMOTE_ROOT/benchmark/bw_mem/bw_mem --dax_dev /dev/interleave_dax --dax_offset 0 --size 256M --parallel 1 --repetition 1 --test bzero >/dev/null 2>&1" || log "WARN: wipe failed"
    local tag="prof$$_${RANDOM}" host
    for i in $(seq 0 $((nh-1))); do
      host=${HOSTS4[$i]}
      local cmd="OMP_NUM_THREADS=$T_PER_HOST ./gapbs_mh/${kern}_mh -g ${G[$kern]} -k 50 --host_id=$i --hosts=$nh --dev=/dev/interleave_dax --shm-mb=${SHM[$kern]} -n 1 ${XTRA[$kern]}"
      gssh "$host" "systemd-run --collect --unit=ae-${tag}-h${i} bash -c 'cd $REMOTE_ROOT && env $cmd >$LOGDIR_REMOTE/${kern}_${cfg}_h${i}.log 2>&1'" >/dev/null \
        || die "[profile/$cfg] $kern: launch failed on $host"
      if [[ $i == 0 ]]; then
        local init_deadline=$(( $(date +%s) + 600 ))
        until gssh "${HOSTS4[0]}" "grep -q 'waiting at initial barrier' $LOGDIR_REMOTE/${kern}_${cfg}_h0.log 2>/dev/null"; do
          (( $(date +%s) < init_deadline )) || die "[profile/$cfg] $kern: rank0 never reached the initial barrier"
          sleep 2
        done
      fi
    done
    local deadline=$(( $(date +%s) + 5400 ))
    until gssh "${HOSTS4[0]}" "grep -q 'Average Time' $LOGDIR_REMOTE/${kern}_${cfg}_h0.log 2>/dev/null"; do
      (( $(date +%s) < deadline )) || { log "WARN: [profile/$cfg] $kern deadline"; break; }
      sleep 5
    done
    sleep 5   # grace for non-initiator ranks to flush SYNC_STATS lines
    for i in $(seq 0 $((nh-1))); do
      scp -q -o BatchMode=yes "root@${HOSTS4[$i]}.dc:$LOGDIR_REMOTE/${kern}_${cfg}_h${i}.log" "$LOGDIR/" 2>/dev/null || true
    done
  done
}


# Wipe shared metadata: stale barrier/segment state from killed runs
# deadlocks the next rendezvous (see global_allocator/reproduce.sh note).
gssh g1 "timeout 120 $REMOTE_ROOT/benchmark/bw_mem/bw_mem --dax_dev /dev/interleave_dax --dax_offset 0 --size 256M --parallel 1 --repetition 1 --test bzero >/dev/null 2>&1" || log "WARN: metadata wipe failed"
run_profiled 2
run_profiled 4

log "restoring clean (uninstrumented) builds"
build_fleet ""

log "analyzing"
python3 "$REPO_ROOT/gapbs_mh/scripts/sync_profile.py" --logs "$LOGDIR" \
  || die "analysis failed"
log "done — gapbs_mh/result/raw/TABLE_VI_QUANT.md"
