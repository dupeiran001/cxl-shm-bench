#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BENCH_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BW_MEM_BIN="$BENCH_ROOT/bw_mem/bw_mem"
RESULT_DIR="$BENCH_ROOT/result/bw_mem/raw"

source "$SCRIPT_DIR/common.sh"

usage() {
  cat <<EOF
Usage: $(basename "$0") <threads_csv> [options]

Options:
  --test <rd|wr|ntwr|rdwr|cp|fwr|frd|fcp|bzero|bcopy>
  --size <bytes|K/M/G/T>
  --repetition <N>
  --cpu <node>
  --mem <node>
  --warmup <N>
  --aligned

Example:
  $(basename "$0") 1,2,4,8 --test rd --size 1G --repetition 10 --cpu 0 --mem 0
EOF
}

threads_csv="${1:-}"
if [[ -z "${threads_csv}" || "${threads_csv}" == "--help" ]]; then
  usage
  exit 1
fi
shift

test="rd"
size="1024000000"
repetition="10"
cpu="0"
mem="0"
warmup="0"
aligned=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --test) test="$2"; shift 2 ;;
    --size) size="$2"; shift 2 ;;
    --repetition) repetition="$2"; shift 2 ;;
    --cpu) cpu="$2"; shift 2 ;;
    --mem) mem="$2"; shift 2 ;;
    --warmup) warmup="$2"; shift 2 ;;
    --aligned) aligned=1; shift ;;
    *) die "unknown arg: $1" ;;
  esac
done

mkdir -p "$RESULT_DIR"
csv="$RESULT_DIR/bw_mem_dram.csv"
ensure_csv_header "$csv" "timestamp,mode,test,size_bytes,repetition,parallel,host,speed_mb_s"

size_bytes="$(size_to_bytes "$size")"
split_csv "$threads_csv" threads

for t in "${threads[@]}"; do
  [[ "$t" =~ ^[0-9]+$ ]] || die "invalid thread count: $t"
  cmd=("$BW_MEM_BIN" --cpu "$cpu" --mem "$mem" --parallel "$t" \
    --repetition "$repetition" --size "$size" --test "$test")
  if [[ "$warmup" != "0" ]]; then
    cmd+=("--warmup" "$warmup")
  fi
  if [[ "$aligned" == "1" ]]; then
    cmd+=("--aligned")
  fi

  output="$("${cmd[@]}" 2>&1)"
  speed="$(extract_speed_mb "$output")"
  [[ -n "$speed" ]] || die "failed to parse speed from output: $output"

  echo "$(timestamp_utc),dram,$test,$size_bytes,$repetition,$t,local,$speed" \
    >>"$csv"
done
