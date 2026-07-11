#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

CONFIG="${CONFIG:-gcc-hooks}"
INPUT="${INPUT:-simlarge}"
THREADS="${THREADS:-16}"
SPLASH2_INPUT="${SPLASH2_INPUT:-$INPUT}"
RESULTS_DIR="${RESULTS_DIR:-results}"
TIMESTAMP="$(date +"%Y-%m-%d_%H-%M-%S")"
OUT="$RESULTS_DIR/run_${TIMESTAMP}"
LOG_DIR="log/amd64-linux.${CONFIG}"

mkdir -p "$OUT/logs" "$OUT/summaries"
MARKER="$OUT/.start_marker"
touch "$MARKER"

# Ensure SPLASH run scripts are executable (they can lose +x after rebuilds).
if [ -d "ext/splash2" ] || [ -d "ext/splash2x" ]; then
  find ext/splash2 ext/splash2x -path "*/inst/*/bin/run.sh" -exec chmod +x {} + 2>/dev/null || true
fi

# Ensure local PARSEC libs are on the runtime search path (helps vips).
LIB_DIRS=(
  "$ROOT/pkgs/libs/libxml2/inst/amd64-linux.${CONFIG}/lib"
  "$ROOT/pkgs/libs/glib/inst/amd64-linux.${CONFIG}/lib"
  "$ROOT/pkgs/libs/zlib/inst/amd64-linux.${CONFIG}/lib"
  "$ROOT/pkgs/libs/hooks/inst/amd64-linux.${CONFIG}/lib"
)
LIB_PATHS=""
for d in "${LIB_DIRS[@]}"; do
  if [ -d "$d" ]; then
    if [ -z "$LIB_PATHS" ]; then
      LIB_PATHS="$d"
    else
      LIB_PATHS="$LIB_PATHS:$d"
    fi
  fi
done
if [ -n "$LIB_PATHS" ]; then
  export LD_LIBRARY_PATH="${LIB_PATHS}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

LIBXML2_DIR="$ROOT/pkgs/libs/libxml2/inst/amd64-linux.${CONFIG}/lib"
shopt -s nullglob
LIBXML2_CANDIDATES=("$LIBXML2_DIR"/libxml2.so.*)
shopt -u nullglob
if [ "${#LIBXML2_CANDIDATES[@]}" -eq 0 ]; then
  echo "[ERROR] Missing libxml2.so.* in: $LIBXML2_DIR" >&2
  echo "[ERROR] Rebuild it with:" >&2
  echo "  ./bin/parsecmgmt -a uninstall -c ${CONFIG} -p parsec.libxml2" >&2
  echo "  ./bin/parsecmgmt -a build -c ${CONFIG} -p parsec.libxml2" >&2
  exit 1
fi

failures=()

run_pkg() {
  local pkg="$1"
  local input="$2"
  # Optional filter: ONLY="parsec.canneal splash2.raytrace ..." runs just the
  # listed packages (default: everything). Used by reproduce.sh for the
  # Table V subset.
  if [[ -n "${ONLY:-}" ]] && ! grep -qw "$pkg" <<<"$ONLY"; then
    return 0
  fi
  echo "[RUN] $pkg (input=$input, threads=$THREADS)"
  if ! ./bin/parsecmgmt -a run -c "$CONFIG" -i "$input" -n "$THREADS" -p "$pkg"; then
    failures+=("$pkg")
  fi
}

KERNELS=(
  parsec.canneal
  parsec.dedup
  parsec.streamcluster
)

APPS=(
  parsec.blackscholes
  parsec.bodytrack
  parsec.facesim
  parsec.ferret
  parsec.fluidanimate
  parsec.freqmine
  parsec.raytrace
  parsec.swaptions
  parsec.vips
  parsec.x264
)

SPLASH2=(
  splash2.barnes
  splash2.cholesky
  splash2.fft
  splash2.fmm
  splash2.lu_cb
  splash2.lu_ncb
  splash2.ocean_cp
  splash2.ocean_ncp
  splash2.radiosity
  splash2.radix
  splash2.raytrace
  splash2.volrend
  splash2.water_nsquared
  splash2.water_spatial
)

SPLASH2X=(
  splash2x.barnes
  splash2x.cholesky
  splash2x.fft
  splash2x.fmm
  splash2x.lu_cb
  splash2x.lu_ncb
  splash2x.ocean_cp
  splash2x.ocean_ncp
  splash2x.radiosity
  splash2x.radix
  splash2x.raytrace
  splash2x.volrend
  splash2x.water_nsquared
  splash2x.water_spatial
)

echo "[INFO] Results will be collected under: $OUT"
echo "[INFO] Using CONFIG=$CONFIG INPUT=$INPUT THREADS=$THREADS SPLASH2_INPUT=$SPLASH2_INPUT"

for pkg in "${KERNELS[@]}"; do
  run_pkg "$pkg" "$INPUT"
done

for pkg in "${APPS[@]}"; do
  run_pkg "$pkg" "$INPUT"
done

for pkg in "${SPLASH2[@]}"; do
  run_pkg "$pkg" "$SPLASH2_INPUT"
done

for pkg in "${SPLASH2X[@]}"; do
  run_pkg "$pkg" "$INPUT"
done

# Collect logs from this run
if [ -d "$LOG_DIR" ]; then
  find "$LOG_DIR" -type f -name "run_*.log" -newer "$MARKER" -print0 | xargs -0 -I{} cp {} "$OUT/logs/" || true
else
  echo "[WARN] Log directory not found: $LOG_DIR"
fi

# Summarize logs into tables
OUT_DIR="$OUT" python3 - <<'PY'
import glob, os, re
from datetime import datetime

out_dir = os.environ.get("OUT_DIR")
log_dir = os.path.join(out_dir, "logs")
logs = glob.glob(os.path.join(log_dir, "run_*.log"))

bench_re = re.compile(r"Benchmarks to run:\s*(\S+)")
roi_re = re.compile(r"Lockstats: ROI duration ([0-9.]+)s")
lock_re = re.compile(r"lock-based sync in ROI: (yes|no) (?:\(acquisitions=([0-9]+), threads=([0-9]+)\)|\(no lock acquisitions\))")
cs_re = re.compile(r"critical-section time sum ([0-9.]+)s, avg ([0-9.]+)us, max ([0-9.]+)us")
ratio_re = re.compile(r"critical-section ratio .* ([0-9.]+)%")
barrier_re = re.compile(r"Barrierstats: barrier waits in ROI: (yes|no) (?:\(waits=([0-9]+), threads=([0-9]+)\)|\(count=0\))")
barrier_time_re = re.compile(r"Barrierstats: wait time sum ([0-9.]+)s, avg ([0-9.]+)us, max ([0-9.]+)us")
barrier_ratio_re = re.compile(r"Barrierstats: wait ratio .* ([0-9.]+)%")
atomic_re = re.compile(r"Atomic ops in ROI: total=([0-9]+), threads=([0-9]+)")
roi_total_re = re.compile(r"Total time spent in ROI: ([0-9.]+)s")

latest = {}
for path in logs:
    base = os.path.basename(path)
    ts_str = base[len("run_"):-len(".log")]
    try:
        ts = datetime.strptime(ts_str, "%Y-%m-%d_%H:%M:%S")
    except ValueError:
        ts = None
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            text = f.read()
    except OSError:
        continue
    m = bench_re.search(text)
    if not m:
        continue
    bench = m.group(1)
    cur = latest.get(bench)
    if cur is None or (ts is not None and cur[0] is not None and ts > cur[0]) or (cur[0] is None and ts is not None):
        latest[bench] = (ts, path, text)

rows = []
atomic_rows = []
for bench, (ts, path, text) in sorted(latest.items()):
    status = "ok"
    if re.search(r"\bError:\b|\bERROR\b|\bFAILED\b", text):
        status = "error"
    roi = ""
    m = roi_re.search(text)
    if m:
        roi = m.group(1)
    else:
        m = roi_total_re.search(text)
        if m:
            roi = m.group(1)

    lock_sync = "no"
    lock_acq = "0"
    lock_threads = "0"
    m = lock_re.search(text)
    if m:
        lock_sync = m.group(1)
        if m.group(2):
            lock_acq = m.group(2)
            lock_threads = m.group(3)

    cs_sum = "0"
    cs_avg = "0"
    cs_max = "0"
    m = cs_re.search(text)
    if m:
        cs_sum, cs_avg, cs_max = m.group(1), m.group(2), m.group(3)

    cs_ratio = "0"
    m = ratio_re.search(text)
    if m:
        cs_ratio = m.group(1)

    barrier_waits = "0"
    barrier_threads = "0"
    m = barrier_re.search(text)
    if m:
        if m.group(2):
            barrier_waits = m.group(2)
            barrier_threads = m.group(3)

    barrier_sum = "0"
    barrier_avg = "0"
    barrier_max = "0"
    m = barrier_time_re.search(text)
    if m:
        barrier_sum, barrier_avg, barrier_max = m.group(1), m.group(2), m.group(3)

    barrier_ratio = "0"
    m = barrier_ratio_re.search(text)
    if m:
        barrier_ratio = m.group(1)

    atomic_total = "0"
    atomic_threads = "0"
    m = atomic_re.search(text)
    if m:
        atomic_total, atomic_threads = m.group(1), m.group(2)

    rows.append({
        "bench": bench,
        "status": status,
        "atomic_total": atomic_total,
        "atomic_threads": atomic_threads,
        "lock_sync": lock_sync,
        "lock_acq": lock_acq,
        "lock_threads": lock_threads,
        "cs_sum": cs_sum,
        "cs_avg": cs_avg,
        "cs_max": cs_max,
        "cs_ratio": cs_ratio,
        "barrier_waits": barrier_waits,
        "barrier_threads": barrier_threads,
        "barrier_sum": barrier_sum,
        "barrier_avg": barrier_avg,
        "barrier_max": barrier_max,
        "barrier_ratio": barrier_ratio,
        "roi": roi,
        "log": os.path.basename(path),
    })

    atomic_rows.append({
        "bench": bench,
        "status": status,
        "atomic_total": atomic_total,
        "atomic_threads": atomic_threads,
        "log": os.path.basename(path),
    })

header = "| Benchmark | Status | Atomic ops (ROI) | Atomic threads | Lock sync | Lock acq | Lock threads | CS sum (s) | CS avg (us) | CS max (us) | CS ratio (%) | Barrier waits | Barrier threads | Barrier sum (s) | Barrier avg (us) | Barrier max (us) | Barrier ratio (%) | ROI (s) | Log |"
sep = "|---|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|"
lines = [header, sep]
for r in rows:
    lines.append("| {bench} | {status} | {atomic_total} | {atomic_threads} | {lock_sync} | {lock_acq} | {lock_threads} | {cs_sum} | {cs_avg} | {cs_max} | {cs_ratio} | {barrier_waits} | {barrier_threads} | {barrier_sum} | {barrier_avg} | {barrier_max} | {barrier_ratio} | {roi} | {log} |".format(**r))

atomic_header = "| Benchmark | Status | Atomic ops (ROI) | Threads | Log |"
atomic_sep = "|---|---|---:|---:|---|"
atomic_lines = [atomic_header, atomic_sep]
for r in atomic_rows:
    atomic_lines.append("| {bench} | {status} | {atomic_total} | {atomic_threads} | {log} |".format(**r))

with open(os.path.join(out_dir, "summaries", "ATOMIC_LOCK_COUNTS.md"), "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
with open(os.path.join(out_dir, "summaries", "ATOMIC_COUNTS.md"), "w", encoding="utf-8") as f:
    f.write("\n".join(atomic_lines))

# Sanity checks
sanity = []
expected_cols = header.count("|") - 1
if sep.count("|") - 1 != expected_cols:
    sanity.append(f"header/sep column mismatch: header={expected_cols} sep={sep.count('|') - 1}")

for r in rows:
    if r["roi"] == "":
        sanity.append(f"missing ROI duration for {r['bench']} (log {r['log']})")
    if r["status"] != "ok":
        sanity.append(f"non-ok status for {r['bench']} (status={r['status']}, log {r['log']})")

sanity_path = os.path.join(out_dir, "summaries", "SANITY.md")
with open(sanity_path, "w", encoding="utf-8") as f:
    if sanity:
        f.write("# Sanity issues\n\n")
        for item in sanity:
            f.write(f"- {item}\n")
    else:
        f.write("# Sanity issues\n\n- none\n")
PY

if ((${#failures[@]})); then
  printf "[WARN] Some benchmarks failed:\\n" | tee "$OUT/summaries/FAILURES.txt"
  printf "%s\\n" "${failures[@]}" | tee -a "$OUT/summaries/FAILURES.txt"
  exit 1
fi

echo "[DONE] Logs in $OUT/logs"
echo "[DONE] Summaries in $OUT/summaries"
