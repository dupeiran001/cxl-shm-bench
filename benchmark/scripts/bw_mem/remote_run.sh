#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

log() {
  local tag="${host_tag:-unknown}"
  echo "[$(date -u +%H:%M:%S)] [$tag] $*" >&2
}

coord=""
coord_user=""
coord_root=""
repo_root=""
csv=""
result_file=""
stop_file=""
start_epoch=""
test=""
size=""
repetition=""
parallel=""
cpu="0"
mem="0"
aligned="0"
warmup="0"
dax_dev=""
dax_offset="0"
host_tag=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --coord) coord="$2"; shift 2 ;;
    --coord_user) coord_user="$2"; shift 2 ;;
    --coord_root) coord_root="$2"; shift 2 ;;
    --repo_root) repo_root="$2"; shift 2 ;;
    --csv) csv="$2"; shift 2 ;;
    --result_file) result_file="$2"; shift 2 ;;
    --stop_file) stop_file="$2"; shift 2 ;;
    --start_epoch) start_epoch="$2"; shift 2 ;;
    --test) test="$2"; shift 2 ;;
    --size) size="$2"; shift 2 ;;
    --repetition) repetition="$2"; shift 2 ;;
    --parallel) parallel="$2"; shift 2 ;;
    --cpu) cpu="$2"; shift 2 ;;
    --mem) mem="$2"; shift 2 ;;
    --aligned) aligned="1"; shift ;;
    --warmup) warmup="$2"; shift 2 ;;
    --dax_dev) dax_dev="$2"; shift 2 ;;
    --dax_offset) dax_offset="$2"; shift 2 ;;
    --host_tag) host_tag="$2"; shift 2 ;;
    *) die "unknown arg: $1" ;;
  esac
done

[[ -n "$coord" ]] || die "--coord required"
[[ -n "$coord_root" ]] || die "--coord_root required"
[[ -n "$repo_root" ]] || die "--repo_root required"
[[ -n "$csv" ]] || die "--csv required"
[[ -n "$result_file" ]] || die "--result_file required"
[[ -n "$stop_file" ]] || die "--stop_file required"
[[ -n "$start_epoch" ]] || die "--start_epoch required"
[[ -n "$test" ]] || die "--test required"
[[ -n "$size" ]] || die "--size required"
[[ -n "$repetition" ]] || die "--repetition required"
[[ -n "$parallel" ]] || die "--parallel required"

if [[ -z "$host_tag" ]]; then
  host_tag="$(hostname -s)"
fi

if [[ -z "$coord_user" ]]; then
  coord_user="root"
fi

[[ "$start_epoch" =~ ^[0-9]+$ ]] || die "--start_epoch must be an epoch integer"

bw_mem_bin="$repo_root/benchmark/bw_mem/bw_mem"
[[ -x "$bw_mem_bin" ]] || die "bw_mem not found at $bw_mem_bin"

now_epoch="$(date -u +%s)"
if (( now_epoch < start_epoch )); then
  sleep_for=$((start_epoch - now_epoch))
  log "waiting ${sleep_for}s for start_epoch=${start_epoch}"
  sleep "$sleep_for"
fi

cmd=("$bw_mem_bin" --cpu "$cpu" --mem "$mem" --parallel "$parallel" \
  --repetition "$repetition" --size "$size" --test "$test")
if [[ "$warmup" != "0" ]]; then
  cmd+=("--warmup" "$warmup")
fi
if [[ "$aligned" == "1" ]]; then
  cmd+=("--aligned")
fi
if [[ -n "$dax_dev" ]]; then
  cmd+=("--dax_dev" "$dax_dev" --dax_offset "$dax_offset")
fi

log "starting bw_mem (parallel=$parallel)"
output="$("${cmd[@]}" 2>&1)"
speed="$(extract_speed_mb "$output")"
[[ -n "$speed" ]] || speed="NaN"

size_bytes="$(size_to_bytes "$size")"
row="$(timestamp_utc),dax_multi,$test,$size_bytes,$repetition,$parallel,$host_tag,$speed"

attempt=0
while true; do
  attempt=$((attempt + 1))
  log "sending result to ${coord} (attempt=${attempt})"
  if ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
    "${coord_user}@${coord}" \
    "$coord_root/benchmark/scripts/bw_mem/append_result.sh" \
    --csv "$csv" \
    --row "$row" \
    --result_file "$result_file" \
    --speed "$speed"; then
    log "result sent"
    break
  fi
  sleep 1
done

log "waiting for stop epoch from ${coord}"
stop_epoch=""
while true; do
  stop_epoch="$(ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new \
    "${coord_user}@${coord}" "cat \"$stop_file\" 2>/dev/null" || true)"
  if [[ "$stop_epoch" =~ ^[0-9]+$ ]]; then
    break
  fi
  sleep 0.2
done

now_epoch="$(date -u +%s)"
if (( now_epoch < stop_epoch )); then
  sleep_for=$((stop_epoch - now_epoch))
  log "waiting ${sleep_for}s for stop_epoch=${stop_epoch}"
  sleep "$sleep_for"
fi
