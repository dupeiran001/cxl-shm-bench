#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO_ROOT="$(cd "$BENCH_ROOT/.." && pwd)"
RESULT_DIR="$BENCH_ROOT/result/bw_mem/raw"

source "$SCRIPT_DIR/common.sh"

log() {
  echo "[$(date -u +%H:%M:%S)] $*" >&2
}

usage() {
  cat <<EOF
Usage: $(basename "$0") <threads_csv> --hosts <host1,host2,...> --dax_dev <path> [options]

Options:
  --hosts <csv>            Comma-separated host list.
  --coord <ip>             Coordinator IP (default: first IP of this host).
  --user <ssh_user>        SSH user (default: root).
  --coord_user <ssh_user>  SSH user for coordinator callbacks (default: --user).
  --coord_root <path>      Repo root on coordinator (default: $REPO_ROOT).
  --remote_root <path>     Repo root on remote hosts (default: $REPO_ROOT).
  --test <rd|wr|ntwr|rdwr|cp|fwr|frd|fcp|bzero|bcopy>
  --size <bytes|K/M/G/T>
  --repetition <N>
  --cpu <node>
  --mem <node>
  --warmup <N>
  --aligned
  --dax_dev <path>
  --dax_offset <bytes|K/M/G/T> (page-aligned base offset)
  --per_host_offset <bytes|K/M/G/T> (page-aligned span per host)
  --start_delay <sec>      Delay before start epoch (default: 5).
  --stop_delay <sec>       Delay before stop epoch (default: 2).

Example:
  $(basename "$0") 1,2,4 --hosts 10.0.0.1,10.0.0.2 --coord 10.0.0.1 \
    --dax_dev /dev/dax0.0 --dax_offset 2G --test rd --size 1G --repetition 10
EOF
}

threads_csv="${1:-}"
if [[ -z "${threads_csv}" || "${threads_csv}" == "--help" ]]; then
  usage
  exit 1
fi
shift

hosts_csv=""
coord="$(hostname -I | awk '{print $1}')"
ssh_user="root"
coord_user=""
coord_root="$REPO_ROOT"
remote_root="$REPO_ROOT"
test="rd"
size="1024000000"
repetition="10"
cpu="0"
mem="0"
warmup="0"
aligned=0
dax_dev=""
dax_offset="0"
per_host_offset=""
start_delay="5"
stop_delay="2"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --hosts) hosts_csv="$2"; shift 2 ;;
    --coord) coord="$2"; shift 2 ;;
    --user) ssh_user="$2"; shift 2 ;;
    --coord_user) coord_user="$2"; shift 2 ;;
    --coord_root) coord_root="$2"; shift 2 ;;
    --remote_root) remote_root="$2"; shift 2 ;;
    --test) test="$2"; shift 2 ;;
    --size) size="$2"; shift 2 ;;
    --repetition) repetition="$2"; shift 2 ;;
    --cpu) cpu="$2"; shift 2 ;;
    --mem) mem="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --aligned) aligned=1; shift ;;
    --dax_dev) dax_dev="$2"; shift 2 ;;
    --dax_offset) dax_offset="$2"; shift 2 ;;
    --per_host_offset) per_host_offset="$2"; shift 2 ;;
    --start_delay) start_delay="$2"; shift 2 ;;
    --stop_delay) stop_delay="$2"; shift 2 ;;
    *) die "unknown arg: $1" ;;
  esac
done

[[ -n "$hosts_csv" ]] || die "--hosts is required"
[[ -n "$dax_dev" ]] || die "--dax_dev is required"
[[ -n "$coord" ]] || die "--coord is required"
[[ "$start_delay" =~ ^[0-9]+$ ]] || die "--start_delay must be an integer"
[[ "$stop_delay" =~ ^[0-9]+$ ]] || die "--stop_delay must be an integer"

if [[ -z "$coord_user" ]]; then
  coord_user="$ssh_user"
fi

mkdir -p "$RESULT_DIR"
csv="$RESULT_DIR/bw_mem_dax_multi_host.csv"
ensure_csv_header "$csv" "timestamp,mode,test,size_bytes,repetition,parallel,host,speed_mb_s"

split_csv "$threads_csv" threads
split_csv "$hosts_csv" hosts
host_count="${#hosts[@]}"
[[ "$host_count" -gt 0 ]] || die "no hosts provided"

page_size="$(getconf PAGESIZE)"
size_bytes="$(size_to_bytes "$size")"
dax_offset_bytes="$(size_to_bytes "$dax_offset")"

for t in "${threads[@]}"; do
  [[ "$t" =~ ^[0-9]+$ ]] || die "invalid thread count: $t"

  buf_len="$(align_up "$size_bytes" "$page_size")"
  if needs_buf2 "$test"; then
    buf2_len="$(align_up "$((size_bytes + 2048))" "$page_size")"
  else
    buf2_len=0
  fi
  per_child_span=$((buf_len + buf2_len))

  if [[ -n "$per_host_offset" ]]; then
    per_host_offset_bytes="$(size_to_bytes "$per_host_offset")"
  else
    per_host_offset_bytes=$((per_child_span * t))
  fi
  if (( per_host_offset_bytes < per_child_span * t )); then
    die "--per_host_offset too small: need >= $((per_child_span * t)) bytes for parallel=$t"
  fi
  if (( per_host_offset_bytes % page_size != 0 )); then
    die "--per_host_offset must be page-aligned (${page_size} bytes)"
  fi

  start_epoch=$(( $(date -u +%s) + start_delay ))
  run_tag="$(date -u +%Y%m%dT%H%M%SZ)_${t}_$$"
  run_dir="$RESULT_DIR/.run_${run_tag}"
  mkdir -p "$run_dir"
  stop_file="$run_dir/stop_epoch"

  log "run parallel=$t start_epoch=$start_epoch delay=${start_delay}s stop_delay=${stop_delay}s"

  ssh_pids=()
  for idx in "${!hosts[@]}"; do
    host="${hosts[$idx]}"
    host_base_offset=$((dax_offset_bytes + idx * per_host_offset_bytes))
    result_file="$run_dir/${host}.speed"
    aligned_arg=()
    if [[ "$aligned" == "1" ]]; then
      aligned_arg=(--aligned)
    fi
    ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
      "${ssh_user}@${host}" \
      "$remote_root/benchmark/scripts/bw_mem/remote_run.sh" \
      --coord "$coord" \
      --coord_user "$coord_user" \
      --coord_root "$coord_root" \
      --repo_root "$remote_root" \
      --csv "$csv" \
      --result_file "$result_file" \
      --stop_file "$stop_file" \
      --start_epoch "$start_epoch" \
      --test "$test" \
      --size "$size" \
      --repetition "$repetition" \
      --parallel "$t" \
      --cpu "$cpu" \
      --mem "$mem" \
      --warmup "$warmup" \
      --dax_dev "$dax_dev" \
      --dax_offset "$host_base_offset" \
      --host_tag "$host" \
      "${aligned_arg[@]}" &
    ssh_pids+=("$!")
    log "launched ${host} offset=${host_base_offset} result_file=${result_file}"
  done

  missing_count=1
  checks=0
  while [[ "$missing_count" -gt 0 ]]; do
    missing_count=0
    for host in "${hosts[@]}"; do
      result_file="$run_dir/${host}.speed"
      if [[ ! -f "$result_file" ]]; then
        missing_count=$((missing_count + 1))
      fi
    done
    checks=$((checks + 1))
    if [[ "$missing_count" -gt 0 ]]; then
      if [[ $((checks % 20)) -eq 1 ]]; then
        log "waiting for ${missing_count} result file(s) from the multi-host bw run (~${checks}s)"
      fi
      sleep 1
    fi
  done

  speeds=()
  for host in "${hosts[@]}"; do
    result_file="$run_dir/${host}.speed"
    read -r speed <"$result_file"
    speeds+=("$speed")
  done

  stop_epoch=$(( $(date -u +%s) + stop_delay ))
  echo "$stop_epoch" >"$stop_file"
  log "wrote stop_epoch=${stop_epoch} to ${stop_file}"

  for pid in "${ssh_pids[@]}"; do
    wait "$pid" || true
  done

  avg="$(printf '%s\n' "${speeds[@]}" | awk '{s+=$1;n+=1} END {if (n>0) printf "%.3f", s/n; else print "NaN"}')"
  echo "$(timestamp_utc),dax_multi,$test,$size_bytes,$repetition,$t,avg,$avg" \
    >>"$csv"
done
