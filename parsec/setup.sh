#!/usr/bin/env bash
# Fetch the PARSEC 3.0 input tarball (archival Zenodo mirror,
# SHA-256-verified), extract it into per-package `inputs/` directories, and
# build the suite with the `gcc-hooks` configuration (the instrumented build
# that feeds Table V).
#
# Overridable via env vars:
#   PARSEC_SIM_URL     - download URL for parsec-3.0-input-sim.tar.gz (490 MB);
#                         defaults to the archival Zenodo mirror
#                         (doi:10.5281/zenodo.21314344). Downloads are
#                         SHA-256-verified.
#   PARSEC_NATIVE_URL  - URL for parsec-3.0-input-native.tar.gz (2.3 GB, optional)
#   PARSEC_INPUT       - which tarball(s) to fetch: "sim" (default),
#                         "native", or "both"
#   PARSEC_BUILD       - "yes" (default) to build after extraction, "no" to skip
#   PARSEC_JOBS        - parallelism for the build (default: $(nproc))
#
# The sim input tarball is downloaded at first use from PARSEC_SIM_URL
# (default: the archival Zenodo mirror of the original Princeton file, which
# is defunct upstream) and SHA-256-verified before extraction. To run
# offline, pre-place the tarball under input-archive/ (or next to this
# script) — a local copy takes precedence over downloading.
# PARSEC_NATIVE_URL (optional native inputs, not used by Table V) has no
# live default — set it to a working mirror if needed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

: "${PARSEC_SIM_URL:=https://zenodo.org/records/21314344/files/parsec-3.0-input-sim.tar.gz?download=1}"
: "${PARSEC_NATIVE_URL:=https://parsec.cs.princeton.edu/download/3.0/parsec-3.0-input-native.tar.gz}"

# Pinned checksum of the sim input tarball as deposited at
# doi:10.5281/zenodo.21314344 (verified against the original Princeton file).
SIM_SHA256=378b3653d8597cad29476b20ff520bdb25b8e90269a3f044e262ff660899fed2
: "${PARSEC_INPUT:=sim}"
: "${PARSEC_BUILD:=yes}"
: "${PARSEC_JOBS:=$(nproc 2>/dev/null || echo 4)}"

die() { echo "[setup] ERROR: $*" >&2; exit 1; }
log() { echo "[setup] $*"; }

command -v curl >/dev/null 2>&1 || die "curl is required"
command -v tar  >/dev/null 2>&1 || die "tar is required"

fetch_and_extract() {
  local tarball="$1"
  local url="$2"
  local preplaced="$ROOT/input-archive/$tarball"
  if [ -f "$tarball" ]; then
    log "$tarball already present; skipping download"
  elif [ -f "$preplaced" ]; then
    # Optional pre-placed copy (offline runs) takes precedence over the
    # download; it is checksum-verified below like any other source.
    log "using pre-placed $preplaced (no download)"
    cp "$preplaced" "$tarball"
  else
    log "downloading $tarball from $url"
    curl -L --fail --progress-bar -o "$tarball.part" "$url" \
      || die "download failed: $url  (set PARSEC_*_URL to a working mirror, or pre-place the tarball at $preplaced)"
    mv "$tarball.part" "$tarball"
  fi
  if [ "$tarball" = "parsec-3.0-input-sim.tar.gz" ]; then
    log "verifying sha256 of $tarball"
    echo "$SIM_SHA256  $tarball" | sha256sum -c --quiet - \
      || die "checksum mismatch for $tarball (expected $SIM_SHA256) — corrupt or wrong file; delete it and re-run"
  fi
  log "extracting $tarball (into per-package inputs/ directories)"
  # the tarball is rooted at parsec-3.0/; strip it so pkgs/*/inputs/ merge into
  # this tree rather than nesting under ./parsec-3.0/
  tar -xzf "$tarball" --strip-components=1
}

case "$PARSEC_INPUT" in
  sim)    fetch_and_extract parsec-3.0-input-sim.tar.gz    "$PARSEC_SIM_URL" ;;
  native) fetch_and_extract parsec-3.0-input-native.tar.gz "$PARSEC_NATIVE_URL" ;;
  both)
    fetch_and_extract parsec-3.0-input-sim.tar.gz    "$PARSEC_SIM_URL"
    fetch_and_extract parsec-3.0-input-native.tar.gz "$PARSEC_NATIVE_URL"
    ;;
  *) die "PARSEC_INPUT must be one of: sim, native, both (got '$PARSEC_INPUT')" ;;
esac

if [ "$PARSEC_BUILD" != "yes" ]; then
  log "skipping build (PARSEC_BUILD=$PARSEC_BUILD)"
  log "done."
  exit 0
fi

# The bundled openssl (pkgs/libs/ssl) was pre-Configured on the original build
# host, so its Makefiles bake an absolute INSTALLTOP. Rewrite that stale prefix
# to the current tree so 'make install' lands libcrypto.a/libssl.a + headers in
# the package's own inst/ dir (openssl is not re-Configured).
find "$ROOT/pkgs/libs/ssl" -type f \( -name Makefile -o -name '*.pc' \) \
  -exec sed -i "s|/home/dpr/Develop/cxl_profiler/parsec|$ROOT|g" {} + 2>/dev/null || true

log "building PARSEC with gcc-hooks (parallelism=$PARSEC_JOBS)"
# The hooks config emits the instrumentation that run_all_simlarge.sh
# later parses into ATOMIC_COUNTS / ATOMIC_LOCK_COUNTS tables.
export MAKEFLAGS="-j${PARSEC_JOBS}"
./bin/parsecmgmt -a build -c gcc-hooks -p libtool -p apps -p kernels -p splash2 -p splash2x

log "done. Next: ./run_all_simlarge.sh"
