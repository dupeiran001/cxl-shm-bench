#!/usr/bin/env bash
# reproduce.sh — one-script reproduction of artifact A3 (benchmark/):
#   Fig 2 bandwidth panels (a)-(f), Fig 3 latency, Table II publish/observe RTT,
#   Table III RDMA-vs-CXL comparison.
#
# Runs FROM the controller (dev box) against the g1..g4 testbed. Hosts must be
# prepared (scripts/prepare.sh) with the shared CXL window mapped
# (scripts/cxl_remap.sh status == healthy). The repo must be deployed and built
# on every host at $REMOTE_ROOT (this script verifies and re-syncs if needed).
#
# Usage:
#   benchmark/reproduce.sh all             # bw + lat + rtt + rdma, RUNS reps
#   benchmark/reproduce.sh bw|lat|rtt|rdma # one section
#   RUNS=3 QUICK=0 benchmark/reproduce.sh bw
#
#   QUICK=1 shrinks thread sweeps + repetitions for a fast validation pass
#   (results still land in raw/ — intended for smoke testing the pipeline).
#
# Output: benchmark/result/{bw_mem,tool_memory_latency,rtt}/raw/ per-panel
# files (the exact names plot.sh expects), then regenerated plots/.
set -uo pipefail

# ------------------------------ configuration -------------------------------
RUNS="${RUNS:-3}"                    # outer repetitions (>=3)
QUICK="${QUICK:-0}"

H_DRAM=g1                            # Fig 2(a) + Fig 3 dram host
H_LOCAL=g1                           # Fig 2(b): host with local CXL modules
LOCAL_DAX="${LOCAL_DAX:-/dev/dax1.0}"   # set to the single-module device
H_REMOTE=g2                          # Fig 2(c): single-host remote sweeps
COORD=g1                             # multi-host coordinator (needs ssh to/from workers)
MULTI2="g1.dc,g2.dc"                 # Fig 2(d)(e)
MULTI4="g1.dc,g2.dc,g3.dc,g4.dc"     # Fig 2(f)
H_RTT_SRV=g1; H_RTT_CLI=g2           # Table II pair

ILDAX_BASE=0x4080000000              # shared window base (must match DVSEC)
ILDAX_CFG_4="0-128:1,128-256:1,256-384:1,384-512:1"   # 4-module page interleave
REMOTE_ROOT=/tmp/cxl_shm_profiling   # repo location on hosts (tmpfs)

THREADS_DRAM="1,2,4,8,16,32,64"
THREADS_DAX="1,2,4,8,16"
REP=10                               # bw_mem inner repetitions
RTT_ITER=1000000
if [[ "$QUICK" == 1 ]]; then THREADS_DRAM="1,8"; THREADS_DAX="1,8"; REP=2; RTT_ITER=100000; fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RAW_BW="$REPO_ROOT/benchmark/result/bw_mem/raw"
RAW_RTT="$REPO_ROOT/benchmark/result/rtt/raw"
SCRIPTS_BW="$REMOTE_ROOT/benchmark/scripts/bw_mem"

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "FATAL: $*"; exit 1; }
gssh() { local h=$1; shift; ssh -4 -n -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=8 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "root@$h.dc" "$@"; }

# ------------------------------- preflight ----------------------------------
preflight() {
  log "preflight: hosts, windows, builds, ssh mesh"
  for h in g1 g2 g3 g4; do
    gssh "$h" true 2>/dev/null || die "$h unreachable — run scripts/cxl_remap.sh"
    gssh "$h" "[ -e /dev/dax0.0 ]" || die "$h missing shared window — run scripts/cxl_remap.sh remap"
    gssh "$h" "[ -x $REMOTE_ROOT/benchmark/bw_mem/bw_mem ]" || die "$h missing built repo at $REMOTE_ROOT"
  done
  gssh "$H_LOCAL" "[ -e $LOCAL_DAX ]" || die "$H_LOCAL missing local CXL $LOCAL_DAX"
  # Full host<->host ssh mesh. The coordinator sshes INTO workers to launch
  # them, and workers ssh BACK to the coordinator to post results
  # (append_result.sh) — both directions must be passwordless.
  local pubs="" pub
  for h in g1 g2 g3 g4; do
    gssh "$h" '[ -f /root/.ssh/id_rsa ] || ssh-keygen -t rsa -N "" -f /root/.ssh/id_rsa -q'
    pub=$(gssh "$h" cat /root/.ssh/id_rsa.pub)
    pubs+="$pub"$'\n'
  done
  for h in g1 g2 g3 g4; do
    gssh "$h" "cat >> /root/.ssh/tmp_mesh <<'EOF'
$pubs
EOF
sort -u /root/.ssh/tmp_mesh /root/.ssh/authorized_keys 2>/dev/null > /root/.ssh/authorized_keys.new
mv /root/.ssh/authorized_keys.new /root/.ssh/authorized_keys; rm -f /root/.ssh/tmp_mesh"
  done
  # verify both directions coordinator<->workers (workers post by IP)
  local coord_ip; coord_ip=$(gssh "$COORD" "hostname -I | awk '{print \$1}'")
  for h in g1 g2 g3 g4; do
    gssh "$COORD" "ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new root@$h.dc true" \
      || die "mesh: $COORD cannot reach $h"
    gssh "$h" "ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new root@$coord_ip true" \
      || die "mesh: $h cannot post back to coordinator ($coord_ip)"
  done
  # NTP sanity (epoch rendezvous)
  for h in g1 g2 g3 g4; do
    gssh "$h" 'chronyc tracking 2>/dev/null | grep -q "Reference ID.*[0-9A-F]"' || log "WARN: $h NTP not syncing"
  done
  # Remove stale fixed-output CSVs from any earlier/aborted run (harvest
  # deletes them after each config; leftovers would mix configs).
  for h in g1 g2 g3 g4; do
    gssh "$h" "rm -f $REMOTE_ROOT/benchmark/result/bw_mem/raw/bw_mem_{dram,dax_local,dax_multi_host}.csv $REMOTE_ROOT/benchmark/result/bw_mem/raw/bw_mem_dax_multi_host.csv; rm -rf $REMOTE_ROOT/benchmark/result/bw_mem/raw/.run_*"
  done
  log "preflight OK"
}

# interleave_dax profile on a set of hosts ("" = unload)
ildax_set() {
  local cfg=$1; shift
  for h in "$@"; do
    gssh "$h" "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; pkill -x lock_bench 2>/dev/null; pkill -x ht_bench 2>/dev/null; pkill -x bw_mem 2>/dev/null; pkill -f '[g]apbs_mh/' 2>/dev/null; sleep 0.5; rmmod interleave_dax 2>/dev/null; true"
    if [[ -n "$cfg" ]]; then
      gssh "$h" "insmod $REMOTE_ROOT/interleave_dax/interleave_dax.ko base_phys=$ILDAX_BASE config=$cfg && [ -e /dev/interleave_dax ]" \
        || die "$h: interleave_dax load failed (cfg=$cfg)"
    fi
  done
}

# Harvest: move rows from a host's fixed output CSV into a per-panel file.
# $1=host  $2=fixed csv name  $3=panel prefix  (splits by test column)
harvest() {
  local h=$1 fixed=$2 prefix=$3 tmp
  tmp=$(mktemp)
  gssh "$h" "cat $REMOTE_ROOT/benchmark/result/bw_mem/raw/$fixed 2>/dev/null" >"$tmp" || true
  [[ -s "$tmp" ]] || die "harvest: no $fixed on $h"
  for test in rd ntwr; do
    local out="$RAW_BW/${prefix}_${test}.csv"
    [[ -f "$out" ]] || head -1 "$tmp" >"$out"
    awk -F, -v t="$test" 'NR>1 && $3==t' "$tmp" >>"$out"
  done
  gssh "$h" "rm -f $REMOTE_ROOT/benchmark/result/bw_mem/raw/$fixed"   # avoid cross-config mixing
  rm -f "$tmp"
}

# ------------------------------- bandwidth ----------------------------------
run_bw() {
  log "=== Fig 2 bandwidth: $RUNS run(s), rep=$REP ==="
  # First reproduction run: preserve the paper-submission baselines once.
  local bak="$RAW_BW/paper_baseline"
  if [[ ! -d "$bak" ]]; then
    mkdir -p "$bak"
    for f in "$RAW_BW"/{dram,dax_local,dax_remote,dax_1host_4cxl,dax_2host_1cxl,dax_2host_4cxl,dax_4host_8cxl,dax_4host_4cxl}_{rd,ntwr}.csv; do
      [[ -f "$f" ]] && mv "$f" "$bak/"
    done
    log "existing raw panels preserved in $bak/"
  fi
  for run in $(seq 1 "$RUNS"); do
    log "--- bw outer run $run/$RUNS ---"

    log "(a) dram on $H_DRAM"
    for t in rd ntwr; do gssh "$H_DRAM" "cd $SCRIPTS_BW && timeout 2400 ./run_dram_local.sh $THREADS_DRAM --test $t --size 1G --repetition $REP --cpu 0 --mem 0" >/dev/null; done
    harvest "$H_DRAM" bw_mem_dram.csv dram

    log "(b) local CXL on $H_LOCAL ($LOCAL_DAX)"
    for t in rd ntwr; do gssh "$H_LOCAL" "cd $SCRIPTS_BW && timeout 2400 ./run_dax_local.sh $THREADS_DAX --dax_dev $LOCAL_DAX --dax_offset 2G --test $t --size 1G --repetition $REP" >/dev/null; done
    harvest "$H_LOCAL" bw_mem_dax_local.csv dax_local

    log "(c1) remote CXL 1 module on $H_REMOTE (raw dax0.0 low offset)"
    for t in rd ntwr; do gssh "$H_REMOTE" "cd $SCRIPTS_BW && timeout 2400 ./run_dax_local.sh $THREADS_DAX --dax_dev /dev/dax0.0 --dax_offset 2G --test $t --size 1G --repetition $REP" >/dev/null; done
    harvest "$H_REMOTE" bw_mem_dax_local.csv dax_remote

    log "(c4) remote CXL 4-module interleave on $H_REMOTE"
    ildax_set "$ILDAX_CFG_4" "$H_REMOTE"
    for t in rd ntwr; do gssh "$H_REMOTE" "cd $SCRIPTS_BW && timeout 2400 ./run_dax_local.sh $THREADS_DAX --dax_dev /dev/interleave_dax --dax_offset 2G --test $t --size 1G --repetition $REP" >/dev/null; done
    harvest "$H_REMOTE" bw_mem_dax_local.csv dax_1host_4cxl

    log "(d) 2 hosts x 1 module (raw dax0.0, disjoint offsets)"
    for t in rd ntwr; do gssh "$COORD" "cd $SCRIPTS_BW && timeout 1200 ./run_dax_multi_host.sh $THREADS_DAX --hosts $MULTI2 --coord \$(hostname -I | awk '{print \$1}') --remote_root $REMOTE_ROOT --coord_root $REMOTE_ROOT --dax_dev /dev/dax0.0 --dax_offset 2G --per_host_offset 20G --test $t --size 1G --repetition $REP" >/dev/null || die "multihost (d) $t failed/timed out"; done
    harvest "$COORD" bw_mem_dax_multi_host.csv dax_2host_1cxl

    log "(e) 2 hosts x 4 modules (interleave)"
    ildax_set "$ILDAX_CFG_4" g1 g2
    for t in rd ntwr; do gssh "$COORD" "cd $SCRIPTS_BW && timeout 1200 ./run_dax_multi_host.sh $THREADS_DAX --hosts $MULTI2 --coord \$(hostname -I | awk '{print \$1}') --remote_root $REMOTE_ROOT --coord_root $REMOTE_ROOT --dax_dev /dev/interleave_dax --dax_offset 2G --per_host_offset 20G --test $t --size 1G --repetition $REP" >/dev/null || die "multihost (e) $t failed/timed out"; done
    harvest "$COORD" bw_mem_dax_multi_host.csv dax_2host_4cxl

    log "(f) 4 hosts x 4 modules (interleave)"
    ildax_set "$ILDAX_CFG_4" g1 g2 g3 g4
    for t in rd ntwr; do gssh "$COORD" "cd $SCRIPTS_BW && timeout 1200 ./run_dax_multi_host.sh $THREADS_DAX --hosts $MULTI4 --coord \$(hostname -I | awk '{print \$1}') --remote_root $REMOTE_ROOT --coord_root $REMOTE_ROOT --dax_dev /dev/interleave_dax --dax_offset 2G --per_host_offset 20G --test $t --size 1G --repetition $REP" >/dev/null || die "multihost (f) $t failed/timed out"; done
    harvest "$COORD" bw_mem_dax_multi_host.csv dax_4host_4cxl
  done
  log "bw raw data complete -> $RAW_BW"
}

# ---------------------------------- rtt -------------------------------------
run_rtt() {
  log "=== Table II RTT on $H_RTT_SRV(server) / $H_RTT_CLI(client), $RUNS run(s) ==="
  ildax_set "$ILDAX_CFG_4" "$H_RTT_SRV" "$H_RTT_CLI"
  mkdir -p "$RAW_RTT"
  local out="$RAW_RTT/rtt_reproduced.md"
  : >"$out"
  for flush in clflushopt clwb; do
    printf '## %s\n\n' "${flush^^}" >>"$out"
    for run in $(seq 1 "$RUNS"); do
      log "rtt $flush run $run/$RUNS"
      # region need to be cleaned before starting,  clean offsets are
      # reliable through both dax0.0 and interleave_dax.
      gssh "$H_RTT_SRV" "pkill -x rtt 2>/dev/null; systemd-run --collect --unit=ae-rtt-srv-\$RANDOM bash -c 'cd $REMOTE_ROOT/benchmark/scripts/rtt && ITER=$RTT_ITER FLUSH=$flush OFFSET=\$((200*1024*1024*1024)) DAX_DEV=/dev/interleave_dax ./server.sh >/tmp/rtt_srv.log 2>&1'"
      sleep 2
      gssh "$H_RTT_CLI" "cd $REMOTE_ROOT/benchmark/scripts/rtt && ITER=$RTT_ITER FLUSH=$flush OFFSET=\$((200*1024*1024*1024)) DAX_DEV=/dev/interleave_dax timeout 600 ./client.sh" >/dev/null 2>&1
      sleep 1
      printf '### run %d\n```\n' "$run" >>"$out"
      gssh "$H_RTT_SRV" "awk '/--- Latency Test Results ---/,/---------------------------/' /tmp/rtt_srv.log" >>"$out"
      gssh "$H_RTT_SRV" "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; true"; gssh "$H_RTT_CLI" "pkill -x rtt 2>/dev/null; pkill -x verify_shm 2>/dev/null; true"
      printf '```\n\n' >>"$out"
    done
  done
  log "rtt results -> $out"
}

# --------------------------------- latency ----------------------------------
run_lat() {
  log "=== Fig 3 latency (automated percentile bisection on $H_LOCAL) ==="
  local raw_lat="$REPO_ROOT/benchmark/result/tool_memory_latency/raw"
  local bak="$raw_lat/paper_baseline"
  if [[ ! -d "$bak" ]]; then
    mkdir -p "$bak"
    for f in "$raw_lat"/{dram,local_cxl,localcxl,remote_cxl}_{rd_miss,store_flush,flush_clean}.csv; do
      [[ -f "$f" ]] && mv "$f" "$bak/"
    done
    log "existing latency raw preserved in $bak/"
  fi
  ildax_set "$ILDAX_CFG_4" "$H_LOCAL"
  local qflag=""; [[ "$QUICK" == 1 ]] && qflag="--quick"
  python3 "$REPO_ROOT/benchmark/scripts/tool_memory_latency/reproduce_lat.py" \
    --host "$H_LOCAL.dc" --local-dax "$LOCAL_DAX" --remote-dax /dev/interleave_dax $qflag \
    || die "latency reproduction failed"
  # figures are (re)generated in the unified plot phase (run_plots).
}

# ------------------------- RDMA comparison (Table III) ----------------------
# Perftest battery between the two BlueField-3 hosts (default g3<->g4) over both
# paths:  ib  = mlx5_0 through the InfiniBand NDR switch (200G/leg);
#         eth = mlx5_1 direct 100G copper (RoCE, no switch hop).
# Latency: write 8/64 B, read 8 B, atomics FAA + CAS; bandwidth: write/read 1 MiB.
# Output: benchmark/result/rdma/raw/rdma.csv (path,test,size,metric,value,run).
# (Merged from the former benchmark/rdma/reproduce.sh so A3 is a single script.)
RDMA_SRV="${SRV:-g3}"; RDMA_CLI="${CLI:-g4}"
ETH_SRV_IP="${ETH_SRV_IP:-10.98.0.3/24}"; ETH_CLI_IP="${ETH_CLI_IP:-10.98.0.4/24}"
ETH_IF="${ETH_IF:-ens257f1np1}"
RDMA_ITER_LAT=10000; RDMA_ITER_BW=2000
RAW_RDMA="$REPO_ROOT/benchmark/result/rdma/raw"
CSV_RDMA="$RAW_RDMA/rdma.csv"

rdma_run_pt() {  # <path> <dev> <target> <tool> <flags...> — parse perftest tables
  local path=$1 dev=$2 target=$3 tool=$4; shift 4
  local extra=("$@") out line
  gssh "$RDMA_SRV" "pkill -f '^$tool' 2>/dev/null; systemd-run --collect --unit=ae-pt-\$RANDOM bash -c '$tool -d $dev ${extra[*]} >/tmp/pt_srv.log 2>&1'"
  sleep 1.5
  out=$(gssh "$RDMA_CLI" "timeout 120 $tool -d $dev ${extra[*]} $target 2>&1") || { log "WARN: $tool failed: $(tail -1 <<<"$out")"; return 1; }
  if grep -q "t_typical" <<<"$out"; then
    line=$(awk '/^ [0-9]/{print; exit}' <<<"$out"); local size typ avg p99
    size=$(awk '{print $1}' <<<"$line"); typ=$(awk '{print $5}' <<<"$line")
    avg=$(awk '{print $6}' <<<"$line"); p99=$(awk '{print $8}' <<<"$line")
    echo "$path,$tool,$size,t_typical_usec,$typ,$RUN" >> "$CSV_RDMA"
    echo "$path,$tool,$size,t_avg_usec,$avg,$RUN"     >> "$CSV_RDMA"
    echo "$path,$tool,$size,p99_usec,$p99,$RUN"       >> "$CSV_RDMA"
    log "  $path $tool ${size}B: typ=${typ}us p99=${p99}us"
  else
    line=$(awk '/^ [0-9]/{print; exit}' <<<"$out"); local size bw
    size=$(awk '{print $1}' <<<"$line"); bw=$(awk '{print $4}' <<<"$line")
    echo "$path,$tool,$size,bw_avg_MBps,$bw,$RUN" >> "$CSV_RDMA"
    log "  $path $tool ${size}B: ${bw} MB/s"
  fi
}
rdma_battery() {  # <path> <dev> <target>
  local path=$1 dev=$2 target=$3
  rdma_run_pt "$path" "$dev" "$target" ib_write_lat -s 8  -n $RDMA_ITER_LAT
  rdma_run_pt "$path" "$dev" "$target" ib_write_lat -s 64 -n $RDMA_ITER_LAT
  rdma_run_pt "$path" "$dev" "$target" ib_read_lat  -s 8  -n $RDMA_ITER_LAT
  rdma_run_pt "$path" "$dev" "$target" ib_atomic_lat -A FETCH_AND_ADD -n $RDMA_ITER_LAT
  rdma_run_pt "$path" "$dev" "$target" ib_atomic_lat -A CMP_AND_SWAP  -n $RDMA_ITER_LAT
  rdma_run_pt "$path" "$dev" "$target" ib_write_bw  -s 1048576 -n $RDMA_ITER_BW
  rdma_run_pt "$path" "$dev" "$target" ib_read_bw   -s 1048576 -n $RDMA_ITER_BW
}
rdma_ib_active() {  # both endpoints' mlx5_0 IB port ACTIVE?
  local h
  for h in "$RDMA_SRV" "$RDMA_CLI"; do
    gssh "$h" "[ \"\$(cat /sys/class/infiniband/mlx5_0/ports/1/state 2>/dev/null)\" = '4: ACTIVE' ]" || return 1
  done
}
run_rdma() {
  log "=== RDMA comparison (Table III): $RDMA_SRV(server) <-> $RDMA_CLI(client) ==="
  mkdir -p "$RAW_RDMA"
  [[ -f "$CSV_RDMA" ]] || echo "path,test,size,metric,value,run" > "$CSV_RDMA"
  { echo "# $(date -u +%FT%TZ) $RDMA_SRV(server) <-> $RDMA_CLI(client)"
    gssh "$RDMA_SRV" "ibv_devinfo 2>/dev/null | grep -E 'hca_id|fw_ver|link_layer'"
    gssh "$RDMA_SRV" "ethtool -m $ETH_IF 2>/dev/null | grep -E 'Identifier|technology' | head -2"
  } > "$RAW_RDMA/env_manifest.txt" 2>/dev/null || true
  local RUN
  for RUN in $(seq 1 "$RUNS"); do
    log "--- RDMA battery run $RUN/$RUNS ---"
    log "-- path: IB switch (mlx5_0, NDR)"
    rdma_ib_active && rdma_battery ib mlx5_0 "$RDMA_SRV.dc" || log "WARN: IB path skipped (mlx5_0 not ACTIVE)"
    log "-- path: direct 100G copper (mlx5_1, RoCE)"
    gssh "$RDMA_SRV" "ip addr add $ETH_SRV_IP dev $ETH_IF 2>/dev/null; ip link set $ETH_IF up"
    gssh "$RDMA_CLI" "ip addr add $ETH_CLI_IP dev $ETH_IF 2>/dev/null; ip link set $ETH_IF up"
    rdma_battery eth "mlx5_1 -R" "${ETH_SRV_IP%/*}" || log "WARN: eth path failed"
  done
  log "RDMA done -> $CSV_RDMA"
}

# ---------------------------------- plots -----------------------------------
run_plots() {  # no args -> both figures; else the named sections only
  local want=("$@"); [[ ${#want[@]} -gt 0 ]] || want=(bw lat)
  log "=== regenerating figures: ${want[*]} ==="
  local w
  for w in "${want[@]}"; do
    case "$w" in
      bw)  (cd "$REPO_ROOT/benchmark/scripts/bw_mem" && ./plot.sh) || log "WARN: bw plot failed" ;;
      lat) (cd "$REPO_ROOT/benchmark/scripts/tool_memory_latency" && ./plot.sh) || log "WARN: lat plot failed" ;;
    esac
  done
  log "figures -> benchmark/result/{bw_mem,tool_memory_latency}/plots/"
}

case "${1:-all}" in
  bw)   preflight; run_bw; run_plots bw ;;
  rtt)  preflight; run_rtt ;;
  lat)  preflight; run_lat; run_plots lat ;;
  rdma) run_rdma ;;
  all)  preflight; run_bw; run_rtt; run_lat; run_rdma; run_plots ;;
  *)    echo "usage: $0 {all|bw|lat|rtt|rdma}  [RUNS=3] [QUICK=1] [SRV=g3 CLI=g4]"; exit 2 ;;
esac
log "done"
