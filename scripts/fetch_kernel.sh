#!/usr/bin/env bash
# fetch_kernel.sh — fetch + prepare the pinned AE kernel source tree used to
# cross-build the A1 kernel module (interleave_dax.ko).
#
# The AE hosts PXE-boot Linux 7.0.0-rc5 built from mainline commit
# 0138af2472df with cfg/config-7.0.0-rc5-cxl. An out-of-tree module must be
# built against a tree at that exact commit with that exact config; this
# script shallow-fetches the single pinned commit (~300 MB), applies the
# shipped config, and runs `make modules_prepare` so `make M=... modules`
# works. It does NOT build the kernel image itself.
#
# Usage:   scripts/fetch_kernel.sh [target-dir]
#          (default target: <repo>/linux-pxe/linux — gitignored)
# Env:     KERNEL_SHA, KERNEL_REMOTE (defaults below), JOBS
# Needs:   git, gcc, make, flex, bison, libelf + openssl dev headers
set -euo pipefail

KERNEL_SHA="${KERNEL_SHA:-0138af2472dfdef0d56fc4697416eaa0ff2589bd}"
KERNEL_REMOTE="${KERNEL_REMOTE:-https://github.com/torvalds/linux.git}"
# canonical alternative: https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET="${1:-$REPO_ROOT/linux-pxe/linux}"
CFG="$REPO_ROOT/cfg/config-7.0.0-rc5-cxl"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

log() { echo "[fetch_kernel] $*"; }
die() { echo "[fetch_kernel] ERROR: $*" >&2; exit 1; }

[ -f "$CFG" ] || die "shipped kernel config not found: $CFG"

# An existing tree (e.g. the fully built standing tree on the AE dev box)
# takes precedence — never touch it.
if [ -f "$TARGET/Makefile" ]; then
  have=$(git -C "$TARGET" rev-parse HEAD 2>/dev/null || echo unknown)
  if [ "$have" = "$KERNEL_SHA" ]; then
    log "kernel tree already present at $TARGET (pinned commit) — nothing to do"
  else
    log "kernel tree already present at $TARGET (HEAD=$have) — leaving it as-is"
    log "(delete it or pass a different target to re-fetch the pinned commit)"
  fi
  exit 0
fi

log "shallow-fetching mainline commit ${KERNEL_SHA:0:12} (~300 MB, one-time)"
mkdir -p "$TARGET"
git -C "$TARGET" init -q
git -C "$TARGET" remote add origin "$KERNEL_REMOTE" 2>/dev/null \
  || git -C "$TARGET" remote set-url origin "$KERNEL_REMOTE"
git -C "$TARGET" fetch --depth 1 origin "$KERNEL_SHA" \
  || die "fetch failed (network? try KERNEL_REMOTE=https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git)"
git -C "$TARGET" checkout -q FETCH_HEAD

log "applying shipped config + preparing for out-of-tree module builds"
cp "$CFG" "$TARGET/.config"
make -C "$TARGET" olddefconfig >/dev/null
make -C "$TARGET" -j"$JOBS" modules_prepare >/dev/null \
  || die "modules_prepare failed (missing build deps? needs flex bison libelf/openssl headers)"

# modpost needs the kernel's exported-symbol table; a prepared-only tree has
# none (it is produced by a full build), so install the one shipped from the
# exact AE kernel build. Without it, module builds would need
# KBUILD_MODPOST_WARN=1 and lose symbol checking.
SYMVERS="$REPO_ROOT/cfg/Module.symvers-7.0.0-rc5-cxl"
if [ -f "$SYMVERS" ]; then
  cp "$SYMVERS" "$TARGET/Module.symvers"
  log "installed shipped Module.symvers (full modpost symbol checking)"
else
  log "WARNING: $SYMVERS not found — build modules with KBUILD_MODPOST_WARN=1"
fi

log "done — build the A1 module with: make -C $TARGET M=$REPO_ROOT/interleave_dax modules"
