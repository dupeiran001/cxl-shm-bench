#!/usr/bin/env bash
set -euo pipefail

die() {
  echo "error: $*" >&2
  exit 1
}

timestamp_utc() {
  date -u +"%Y-%m-%dT%H:%M:%SZ"
}

split_csv() {
  local list="$1"
  local -n _out="$2"
  IFS=',' read -r -a _out <<<"$list"
}

size_to_bytes() {
  local s="$1"
  local last="${s: -1}"
  local num scale
  if [[ "$last" =~ [kKmMgGtT] ]]; then
    num="${s:0:${#s}-1}"
    case "$last" in
      k|K) scale=1024 ;;
      m|M) scale=$((1024**2)) ;;
      g|G) scale=$((1024**3)) ;;
      t|T) scale=$((1024**4)) ;;
      *) scale=1 ;;
    esac
  else
    num="$s"
    scale=1
  fi
  [[ "$num" =~ ^[0-9]+$ ]] || die "invalid size: $s"
  echo $((num * scale))
}

align_up() {
  local size="$1"
  local align="$2"
  echo $((((size + align - 1) / align) * align))
}

needs_buf2() {
  local test="$1"
  case "$test" in
    cp|fcp|bcopy) return 0 ;;
    *) return 1 ;;
  esac
}

extract_speed_mb() {
  local text="$1"
  echo "$text" | sed -n 's/.*speed is \([0-9.]\+\) MB\/s.*/\1/p' | tail -n1
}

ensure_csv_header() {
  local csv="$1"
  local header="$2"
  if [[ ! -f "$csv" ]]; then
    echo "$header" >"$csv"
  fi
}
