#!/usr/bin/env bash
# reproduce.sh — one-script reproduction of artifact A7 (hashtable_mh/):
#   concurrent hash table on shared CXL memory (reviewer-requested
#   non-graph / shared-data-structure workload) across local DRAM,
#   remote CXL, 2-host shared CXL, and 4-host shared CXL.
#
# SSH-orchestrated from the controller (dev box) against the g1..g4 testbed;
# the benchmark itself rendezvouses via the shared-memory barrier internally.
#
# Config matrix:
#   configs  dram (tmpfs file), remote_cxl (/dev/dax0.0),
#            shared_2hosts + shared_4hosts (/dev/interleave_dax)
#   mixes    100 / 95 / 50 % reads      threads/host  8 / 16 / 32
#   table    2^20 buckets, 1M preloaded keys, 10 s per point, RUNS=3 outer
#
# Usage:  hashtable_mh/reproduce.sh [all|dram|remote|shared2|shared4]
#         RUNS=3 QUICK=0 hashtable_mh/reproduce.sh all
set -uo pipefail

RUNS="${RUNS:-3}"
QUICK="${QUICK:-0}"
REMOTE_ROOT=/tmp/cxl_shm_profiling
ILDAX_BASE=0x4080000000
ILDAX_CFG_4="0-128:1,128-256:1,256-384:1,384-512:1"
H_SINGLE=g2                          # dram + remote_cxl host
HOSTS4=(g1 g2 g3 g4)
THREADS=(8 16 32)                    # per host
MIXES=(100 95 50)                    # read percentage
DURATION=10                          # seconds per point
BUCKETS=$((1 << 20))
KEYS=1000000
SLAB_MB=256
SHM_MB=4096
DRAM_DEV=/dev/shm/ht_dram.bin        # tmpfs-backed segment for the dram config
DRAM_SIZE=6G
POINT_TIMEOUT=300                    # timeout per remote benchmark invocation

if [[ "$QUICK" == 1 ]]; then
  THREADS=(8); MIXES=(50); DURATION=2
  BUCKETS=65536; KEYS=100000; SLAB_MB=64; SHM_MB=1024
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CSV="$REPO_ROOT/hashtable_mh/result/raw/hashtable.csv"
HDR="benchmark,hosts,threads_per_host,mix,run,mops,p50_ns,p95_ns,p99_ns"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "FATAL: $*"; exit 1; }
gssh() { local h=$1; shift; ssh -4 -n -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "root@$h.dc" "$@"; }

csv_row() { echo "$1" >> "$CSV"; }

# Kill leftover benchmark processes everywhere (used on any point failure).
kill_stragglers() {
  for h in "${HOSTS4[@]}"; do
    gssh "$h" "pkill -f ht_bench 2>/dev/null; true"
  done
}

# "mops,p50,p95,p99" from a host-0 log, empty on parse failure.
parse_agg() {
  awk '/^AGGREGATE /{for(i=1;i<=NF;i++){split($i,kv,"=");
         if(kv[1]=="mops")m=kv[2]; if(kv[1]=="p50_ns")a=kv[2];
         if(kv[1]=="p95_ns")b=kv[2]; if(kv[1]=="p99_ns")c=kv[2]}}
       END{if(m!="")printf "%s,%s,%s,%s",m,a,b,c}'
}

preflight() {
  for h in "${HOSTS4[@]}"; do
    gssh "$h" "[ -x $REMOTE_ROOT/hashtable_mh/ht_bench ]" \
      || die "$h: ht_bench missing (deploy repo; make -C hashtable_mh)"
    gssh "$h" "[ -e /dev/dax0.0 ]" \
      || die "$h: shared window missing — run scripts/cxl_remap.sh"
  done
  local bak="$REPO_ROOT/hashtable_mh/result/raw/paper_baseline"
  if [[ ! -d "$bak" ]]; then
    mkdir -p "$bak"; cp "$CSV" "$bak/" 2>/dev/null || true
    log "previous hashtable.csv (if any) preserved in $bak/"
  fi
  [[ -f "$CSV" ]] || echo "$HDR" > "$CSV"
}

ildax_load() { # $@ = hosts to load the 4-module interleave window on
  for h in "$@"; do
    gssh "$h" "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; pkill -x lock_bench 2>/dev/null; pkill -x ht_bench 2>/dev/null; pkill -x bw_mem 2>/dev/null; pkill -f '[g]apbs_mh/' 2>/dev/null; sleep 0.5; rmmod interleave_dax 2>/dev/null; insmod $REMOTE_ROOT/interleave_dax/interleave_dax.ko base_phys=$ILDAX_BASE config=$ILDAX_CFG_4 && [ -e /dev/interleave_dax ]" \
      || die "$h: interleave_dax load failed"
  done
}

bench_flags() { # $1=threads $2=mix
  echo "--threads=$1 --mix=$2 --buckets=$BUCKETS --keys=$KEYS --slab-mb=$SLAB_MB --shm-mb=$SHM_MB --seconds=$DURATION"
}

run_single() {  # $1=config(dram|remote_cxl) $2=run index
  local cfg=$1 run=$2 dev
  if [[ "$cfg" == dram ]]; then
    dev=$DRAM_DEV
  else
    dev=/dev/dax0.0
  fi
  for mix in "${MIXES[@]}"; do
    for t in "${THREADS[@]}"; do
      log "[$cfg] mix=$mix t=$t (run $run)"
      if [[ "$cfg" == dram ]]; then
        gssh "$H_SINGLE" "rm -f $DRAM_DEV && truncate -s $DRAM_SIZE $DRAM_DEV" \
          || die "[$cfg] cannot create tmpfs segment"
      fi
      local cmd="./hashtable_mh/ht_bench --dev=$dev --hosts=1 --host_id=0 --init $(bench_flags "$t" "$mix")"
      local out; out=$(mktemp)
      gssh "$H_SINGLE" "cd $REMOTE_ROOT && timeout $POINT_TIMEOUT $cmd 2>/dev/null" > "$out"
      local rc=$?
      local vals; vals=$(parse_agg < "$out")
      local ok=1
      grep -q 'errors_all=0' "$out" || ok=0
      rm -f "$out"
      if [[ $rc != 0 || -z "$vals" || $ok != 1 ]]; then
        kill_stragglers
        die "[$cfg] mix=$mix t=$t failed (rc=$rc vals='$vals' ok=$ok)"
      fi
      csv_row "$cfg,1,$t,$mix,$run,$vals"
    done
  done
  [[ "$cfg" == dram ]] && gssh "$H_SINGLE" "rm -f $DRAM_DEV"
}

run_shared() {  # $1=nhosts(2|4) $2=run index
  local nh=$1 run=$2
  local cfg="shared_${nh}hosts"
  ildax_load "${HOSTS4[@]:0:$nh}"
  # Wipe shared metadata (stale barrier/segment state from killed runs
  # deadlocks the rendezvous — see global_allocator/reproduce.sh note).
  gssh g1 "timeout 120 $REMOTE_ROOT/benchmark/bw_mem/bw_mem --dax_dev /dev/interleave_dax --dax_offset 0 --size 256M --parallel 1 --repetition 1 --test bzero >/dev/null 2>&1" || log "WARN: metadata wipe failed"
  for mix in "${MIXES[@]}"; do
    for t in "${THREADS[@]}"; do
      log "[$cfg] mix=$mix t/host=$t (run $run)"
      local outdir; outdir=$(mktemp -d)
      # systemd-run launches + rank0-completion poll (locally backgrounded ssh
      # launches proved unreliable; ht_bench's attach path retries, so rank
      # ordering only needs the initializer first).
      local tag="ht$$_${RANDOM}" host
      for i in $(seq 0 $((nh - 1))); do
        host=${HOSTS4[$i]}
        local initflag=""; [[ $i == 0 ]] && initflag="--init"
        local cmd="./hashtable_mh/ht_bench --dev=/dev/interleave_dax --hosts=$nh --host_id=$i $initflag $(bench_flags "$t" "$mix")"
        gssh "$host" "systemd-run --collect --unit=ae-${tag}-h${i} bash -c 'cd $REMOTE_ROOT && timeout $POINT_TIMEOUT env $cmd >/tmp/${tag}_h${i}.log 2>&1'" >/dev/null \
          || die "[$cfg] mix=$mix t=$t: launch failed on $host"
        [[ $i == 0 ]] && sleep 3
      done
      local deadline=$(( $(date +%s) + POINT_TIMEOUT + 60 ))
      until gssh "${HOSTS4[0]}" "grep -q '^AGGREGATE' /tmp/${tag}_h0.log 2>/dev/null"; do
        (( $(date +%s) < deadline )) || break
        sleep 5
      done
      sleep 3   # grace for non-initiator ranks to print their blocks
      local rc=0
      for i in $(seq 0 $((nh - 1))); do
        scp -q -o BatchMode=yes "root@${HOSTS4[$i]}.dc:/tmp/${tag}_h${i}.log" "$outdir/h$i.log" 2>/dev/null || rc=1
        gssh "${HOSTS4[$i]}" "rm -f /tmp/${tag}_h${i}.log; systemctl stop ae-${tag}-h${i} 2>/dev/null; true"
      done
      local vals; vals=$(parse_agg < "$outdir/h0.log" 2>/dev/null)
      local errs; errs=$(grep -h '^error_count:' "$outdir"/h*.log 2>/dev/null | awk '{s+=$2} END{print s+0}')
      rm -rf "$outdir"
      if [[ $rc != 0 || -z "$vals" || "$errs" != 0 ]]; then
        kill_stragglers
        die "[$cfg] mix=$mix t=$t failed (rc=$rc vals='$vals' errors=$errs)"
      fi
      csv_row "$cfg,$nh,$t,$mix,$run,$vals"
    done
  done
}

run_plots() {
  (cd "$REPO_ROOT/hashtable_mh/scripts" && python3 plot_ht.py) || log "WARN: plot failed"
}

main() {
  preflight
  for run in $(seq 1 "$RUNS"); do
    log "===== hashtable pass $run/$RUNS ====="
    case "${1:-all}" in
      dram)    run_single dram "$run" ;;
      remote)  run_single remote_cxl "$run" ;;
      shared2) run_shared 2 "$run" ;;
      shared4) run_shared 4 "$run" ;;
      all)     run_single dram "$run"; run_single remote_cxl "$run"
               run_shared 2 "$run"; run_shared 4 "$run" ;;
      *) die "usage: $0 [all|dram|remote|shared2|shared4]" ;;
    esac
  done
  run_plots
  log "done -> $CSV"
}
main "${1:-all}"
