#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/common.sh"

csv=""
row=""
result_file=""
speed=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --csv) csv="$2"; shift 2 ;;
    --row) row="$2"; shift 2 ;;
    --result_file) result_file="$2"; shift 2 ;;
    --speed) speed="$2"; shift 2 ;;
    *) die "unknown arg: $1" ;;
  esac
done

[[ -n "$csv" ]] || die "--csv required"
[[ -n "$row" ]] || die "--row required"

mkdir -p "$(dirname "$csv")"
lock="${csv}.lock"

{
  flock -x 9
  if [[ ! -f "$csv" ]]; then
    echo "timestamp,mode,test,size_bytes,repetition,parallel,host,speed_mb_s" \
      >"$csv"
  fi
  echo "$row" >>"$csv"
} 9>"$lock"

if [[ -n "$result_file" && -n "$speed" ]]; then
  mkdir -p "$(dirname "$result_file")"
  echo "$speed" >"$result_file"
fi
