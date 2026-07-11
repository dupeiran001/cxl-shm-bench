#!/usr/bin/env bash
# cxl_remap.sh — bring the shared CXL testbed to a known-good state.
#
# Implements the reliable recovery procedure for the H3 Falcon (XConn) CXL
# memory server + g1..g4 hosts:
#
#   status   read-only health report (H3 port/DVSEC state + per-host devices)
#   remap    full cycle: power off all hosts -> reboot H3 -> push the bundled
#            pool map (scripts/poolmap/Poolmap_sc_ae.sh) to /eMMCRS/Poolmap_sc_ae.sh
#            and run it (self-contained; does NOT depend on the shared
#            /eMMCRS/Poolmap.sh) -> power on hosts -> prepare.sh -> verify;
#            retries the cycle on failure, then prints the manual escalation
#            (physical H3 replug) if still unhealthy.
#
# Usage:
#   scripts/cxl_remap.sh status
#   scripts/cxl_remap.sh remap [--yes] [--max-retries N]
#
# The remap cycle POWERS OFF ALL g-HOSTS. It refuses to start without --yes
# or an interactive confirmation.
#
# Credentials/topology defaults below describe the paper's AE testbed and are
# env-overridable (H3_ADDR/H3_USER/H3_PASS/HOST_PASS/CXL_BASE).
set -uo pipefail

# ------------------------------- configuration -------------------------------
H3_ADDR="${H3_ADDR:-192.168.128.250}"
H3_USER="${H3_USER:-admin}"
H3_PASS="${H3_PASS:-h3}"
H3_POOLMAP="/eMMCRS/Poolmap.sh"                                  # shared default map on H3 (reference/status only)
H3_POOLMAP_SCAE="${H3_POOLMAP_SCAE:-/eMMCRS/Poolmap_sc_ae.sh}"   # our dedicated map: remap pushes + runs THIS

HOSTS=(g1 g2 g3 g4)                    # ipmi names; ssh via <name>.dc
declare -A EP_PORT=([g1]=8 [g2]=9 [g3]=10 [g4]=11)
HOST_PASS="${HOST_PASS:-123456}"       # first-boot root password (tmpfs image)

# PXE boot control: before power-cycling, point g1..g4 at the "Debian Peiran
# SC_AE" (production, debug-OFF) menuentry on the SHARED grub.cfg. The per-MAC
# default index can drift as others use the shared file, so pxe_set_scae() reads
# it, rewrites ONLY the four g-host 'set default' lines, and pushes it back
# safely (see the function). Set SKIP_GRUB=1 to bypass.
PXE_ADDR="${PXE_ADDR:-pxe.dc}"; PXE_USER="${PXE_USER:-root}"
PXE_GRUB="${PXE_GRUB:-/srv/tftp/grub/grub.cfg}"
SCAE_ENTRY="${SCAE_ENTRY:-1}"                              # grub 'set default' index of the SC_AE entry
SCAE_MENU_MATCH="${SCAE_MENU_MATCH:-Debian Peiran SC_AE}"  # sanity: menuentry #SCAE_ENTRY must contain this
declare -A G_MAC=([g1]=90:5a:08:9c:32:2c [g2]=90:5a:08:9c:2e:cc \
                  [g3]=90:5a:08:9c:32:3c [g4]=90:5a:08:9c:2e:a8)

# Desired shared mapping: same modules to every EP port, linear, consecutive.
CXL_BASE="${CXL_BASE:-0x4080000000}"   # must match host DVSEC (re-check if BIOS/memory changes)
MODULE_DSPORTS=(5 21 4 20)             # downstream ports, mapped in this order
MODULE_SIZE_G=128
WINDOW_SIZE_G=$(( MODULE_SIZE_G * ${#MODULE_DSPORTS[@]} ))   # 512

# Per-host device expectations
declare -A WANT_LOCAL_CXL=([g1]=1 [g2]=0 [g3]=0 [g4]=0)   # g1 only: 2x local Micron modules (dax1.0)
declare -A WANT_BF3=([g1]=0 [g2]=0 [g3]=1 [g4]=1)         # BlueField-3 + IB uplink
DIMMS_EXPECTED=8
SOCKETS_EXPECTED=1

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREPARE_SH="$REPO_ROOT/scripts/prepare.sh"
BUNDLED_POOLMAP="${BUNDLED_POOLMAP:-$REPO_ROOT/scripts/poolmap/Poolmap_sc_ae.sh}"  # bundled H3 switch map (cloned from H3)
BOOT_POLL_TRIES=60        # x 8s  = 8 min per host
H3_POLL_TRIES=40          # x 8s  = ~5.3 min for H3 reboot
LOG_FILE="${LOG_FILE:-/tmp/cxl_remap_$(date +%Y%m%d_%H%M%S).log}"

# --------------------------------- helpers ----------------------------------
log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$LOG_FILE"; }
h3()   { sshpass -p "$H3_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=6 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$H3_USER@$H3_ADDR" "$@" 2>/dev/null; }
h3_put() { sshpass -p "$H3_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=8 "$H3_USER@$H3_ADDR" "cat > $1"; }   # write stdin to H3 path $1
gssh() { local h=$1; shift; ssh -4 -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=5 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "root@$h.dc" "$@" 2>/dev/null; }
pssh() { ssh -4 -n -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=4 "$PXE_USER@$PXE_ADDR" "$@"; }

# DVSEC health for one EP port. Output: "ok <base>" or "bad <reason>".
h3_dvsec_check() {
  local port=$1 out status_word row3 hi lo base
  out=$(h3 "xconn-control showdvsec $port")
  [[ -z "$out" ]] && { echo "bad no-dvsec-output"; return; }
  # DVSEC 7 line, 4th data word: chars 2-3 must be "26" (set after BIOS POST).
  status_word=$(awk '/DVSEC 7:/ {print $8}' <<<"$out")
  # DVSEC 0 third continuation row holds the 64-bit CXL base (hi lo).
  row3=$(awk '/DVSEC 0/{n++} n==3{print $(NF-3), $(NF-2); exit}' <<<"$out")
  hi=${row3%% *}; lo=${row3##* }
  base=$(( (16#${hi:-0} << 32) | 16#${lo:-0} ))
  if [[ "${status_word:2:2}" != "26" ]]; then
    echo "bad status=${status_word:-none}"
  elif (( base == 0 )); then
    echo "bad base=0"
  else
    printf 'ok 0x%x\n' "$base"
  fi
}

h3_portstatus() {  # -> link state string, e.g. L0 / DETECT_QUIET / RCVRY_...
  h3 "xconn-control portstatus $1" | awk -F': *' '/Link:/{print $2; exit}' | tr -d ' '
}

# Verify Poolmap.sh maps every EP port to the desired modules/addresses.
h3_poolmap_verify() {
  local pm="${1:-$H3_POOLMAP_SCAE}"
  local content ep i want_addr fails=0
  content=$(h3 "grep -E '^[[:space:]]*xconn-control (poolmap|sethdmsize)' $pm")
  [[ -z "$content" ]] && { log "H3: cannot read $pm"; return 1; }
  for h in "${HOSTS[@]}"; do
    ep=${EP_PORT[$h]}
    for i in "${!MODULE_DSPORTS[@]}"; do
      want_addr=$(printf '0x%x' $(( CXL_BASE + i * MODULE_SIZE_G * 1024*1024*1024 )))
      if ! grep -qE "poolmap[[:space:]]+${MODULE_DSPORTS[$i]}[[:space:]]+${MODULE_SIZE_G}[[:space:]]+0[[:space:]]+${ep}[[:space:]]+${want_addr}" <<<"$content"; then
        log "Poolmap.sh MISMATCH: ep=$ep expected 'poolmap ${MODULE_DSPORTS[$i]} $MODULE_SIZE_G 0 $ep $want_addr'"
        fails=$((fails+1))
      fi
    done
    grep -qE "sethdmsize[[:space:]]+${ep}[[:space:]]+${WINDOW_SIZE_G}" <<<"$content" \
      || { log "Poolmap.sh MISMATCH: ep=$ep expected 'sethdmsize $ep $WINDOW_SIZE_G'"; fails=$((fails+1)); }
  done
  return $fails
}

host_power() { ipmi "$1" "$2" >/dev/null 2>&1; }
host_power_state() { ipmi "$1" status 2>/dev/null | awk -F': ' '/Power state/{print $2; exit}'; }

wait_host_ssh() {  # install key (fresh tmpfs) and wait for ssh
  local h=$1 i
  for i in $(seq 1 "$BOOT_POLL_TRIES"); do
    sshpass -p "$HOST_PASS" ssh-copy-id -o StrictHostKeyChecking=no -o ConnectTimeout=4 "root@$h.dc" >/dev/null 2>&1
    gssh "$h" true && return 0
    sleep 8
  done
  return 1
}

# Per-host device verification. Prints failures; returns count.
host_verify() {
  local h=$1 fails=0 out
  out=$(gssh "$h" '
    echo "sockets=$(lscpu | awk -F: "/^Socket/{gsub(/ /,\"\",\$2);print \$2}")"
    echo "dimms=$(dmidecode -t 17 2>/dev/null | grep -cE "^\s+Size: [0-9]+ GB")"
    echo "srat_remote=$(dmesg | grep -c "SRAT:.*'"${CXL_BASE#0x}"'")"
    ndax=0; daxes=""
    for d in /sys/bus/dax/devices/dax*; do
      [ -e "$d" ] || continue
      ndax=$((ndax+1)); daxes="$daxes $(basename $d):$(numfmt --to=iec <$d/size)"
    done
    echo "ndax=$ndax"
    echo "daxes=${daxes# }"
    echo "bf3=$(grep -l 0xa2dc /sys/bus/pci/devices/*/device 2>/dev/null | wc -l)"
    echo "ib=$(cat /sys/class/infiniband/mlx5_0/ports/1/link_layer 2>/dev/null)"')
  [[ -z "$out" ]] && { log "$h: UNREACHABLE"; return 9; }
  local sockets dimms srat_remote ndax daxes bf3 ib
  ndax=$(awk -F= '/^ndax=/{print $2}' <<<"$out")
  sockets=$(awk -F= '/^sockets=/{print $2}' <<<"$out")
  dimms=$(awk -F= '/^dimms=/{print $2}' <<<"$out")
  srat_remote=$(awk -F= '/^srat_remote=/{print $2}' <<<"$out")
  daxes=$(awk -F= '/^daxes=/{print $2}' <<<"$out")
  bf3=$(awk -F= '/^bf3=/{print $2}' <<<"$out")
  ib=$(awk -F= '/^ib=/{print $2}' <<<"$out")
  soft_reserved=$(awk -F= '/^soft_reserved=/{print $2}' <<<"$out")

  [[ "$sockets" == "$SOCKETS_EXPECTED" ]] || { log "$h: sockets=$sockets (want $SOCKETS_EXPECTED)"; fails=$((fails+1)); }
  [[ "$dimms" == "$DIMMS_EXPECTED" ]]     || { log "$h: dimms=$dimms (want $DIMMS_EXPECTED)"; fails=$((fails+1)); }
  (( srat_remote > 0 )) || { log "$h: no SRAT node at $CXL_BASE (remote CXL window missing)"; fails=$((fails+1)); }
  grep -q "${WINDOW_SIZE_G}G" <<<"$daxes" || { log "$h: no ${WINDOW_SIZE_G}G dax window (daxes: ${daxes:-none})"; fails=$((fails+1)); }
  if [[ "${WANT_LOCAL_CXL[$h]}" == 1 ]]; then
    # remote window is one dax device; local modules appear as additional ones
    (( ndax >= 2 )) || { log "$h: local CXL modules missing (dax devices: $ndax — expected remote + local)"; fails=$((fails+1)); }
  fi
  if [[ "${WANT_BF3[$h]}" == 1 ]]; then
    (( bf3 >= 1 )) || { log "$h: BlueField-3 not on PCI bus"; fails=$((fails+1)); }
    [[ "$ib" == "InfiniBand" ]] || { log "$h: mlx5_0 link_layer='$ib' (want InfiniBand)"; fails=$((fails+1)); }
  fi
  (( fails == 0 )) && log "$h: OK (daxes: $daxes)"
  return $fails
}

# --------------------------------- commands ---------------------------------
cmd_status() {
  log "=== H3 ($H3_ADDR) ==="
  for h in "${HOSTS[@]}"; do
    local ep=${EP_PORT[$h]}
    log "$h ep$ep: link=$(h3_portstatus "$ep") dvsec=$(h3_dvsec_check "$ep") power=$(host_power_state "$h")"
  done
  for pm in "$H3_POOLMAP_SCAE" "$H3_POOLMAP"; do
    h3_poolmap_verify "$pm" && log "$pm content: OK (${#MODULE_DSPORTS[@]} modules -> all EP ports, base $CXL_BASE)" \
                            || log "$pm content: MISMATCHES above (or file absent)"
  done
  log "=== hosts ==="
  local total=0
  for h in "${HOSTS[@]}"; do host_verify "$h"; total=$((total+$?)); done
  log "=== status done (host failure count: $total) ==="
  return "$total"
}

# Point the g-hosts' PXE default at the "Debian Peiran SC_AE" (production,
# debug-OFF) menuentry on the SHARED grub.cfg, before power-cycling. Safe by
# construction: fetch -> edit LOCALLY -> verify (menuentry #SCAE_ENTRY really is
# the SC_AE entry; all four g-MAC blocks located; the diff touches ONLY g1-g4
# 'set default' lines, at most 8) -> timestamped backup on pxe -> atomic push ->
# confirm the live file; restores the backup on any mismatch and pushes NOTHING
# if the change can't be verified. Non-g MAC blocks are never modified.
# Returns 0 on success (incl. "already correct"), non-zero otherwise.
pxe_set_scae() {
  log "PXE grub: set ${HOSTS[*]} default -> entry $SCAE_ENTRY ('$SCAE_MENU_MATCH') on $PXE_USER@$PXE_ADDR:$PXE_GRUB"
  local ts orig new diffout nchg nsd
  ts=$(date +%Y%m%d-%H%M%S); orig=$(mktemp); new=$(mktemp)
  pssh "cat $PXE_GRUB" > "$orig" || { log "  cannot read $PXE_GRUB on $PXE_ADDR"; rm -f "$orig" "$new"; return 1; }
  [[ -s "$orig" ]] || { log "  fetched grub.cfg is empty — refusing"; rm -f "$orig" "$new"; return 1; }
  pssh "cp -a $PXE_GRUB $PXE_GRUB.bak.cxlremap-$ts" || { log "  backup on $PXE_ADDR failed — refusing"; rm -f "$orig" "$new"; return 1; }

  if ! SCAE_ENTRY="$SCAE_ENTRY" SCAE_MENU_MATCH="$SCAE_MENU_MATCH" \
       G1="${G_MAC[g1]}" G2="${G_MAC[g2]}" G3="${G_MAC[g3]}" G4="${G_MAC[g4]}" \
       python3 - "$orig" "$new" <<'PY'
import sys, os, re
src, dst = sys.argv[1], sys.argv[2]
entry = os.environ["SCAE_ENTRY"]; match = os.environ["SCAE_MENU_MATCH"]
MACS = {"g1": os.environ["G1"], "g2": os.environ["G2"],
        "g3": os.environ["G3"], "g4": os.environ["G4"]}
lines = open(src).read().split("\n")
menu = [l for l in lines if l.strip().startswith("menuentry ")]
idx = int(entry)
if idx < 0 or idx >= len(menu) or match not in menu[idx]:
    sys.exit("ABORT: menuentry #%s is not %r (got %r)"
             % (entry, match, menu[idx] if 0 <= idx < len(menu) else None))
seen = set()
for i, l in enumerate(lines):
    if "net_default_mac" not in l:
        continue
    for h, mac in MACS.items():
        if h in seen or mac.lower() not in l.lower():
            continue
        for j in range(i + 1, min(i + 6, len(lines))):
            if "set default=" in lines[j]:
                lines[j] = re.sub(r'set default="[0-9]*"',
                                  'set default="%s"' % entry, lines[j])
                seen.add(h)
                break
        break
if seen != set(MACS):
    sys.exit("ABORT: did not locate all g1-g4 net_default_mac blocks; found only %s"
             % sorted(seen))
open(dst, "w").write("\n".join(lines))
PY
  then
    log "  grub edit/verify failed (reason above) — NOTHING pushed"; rm -f "$orig" "$new"; return 1
  fi

  diffout=$(diff "$orig" "$new" || true)
  nchg=$(grep -cE '^[<>]' <<<"$diffout"); nsd=$(grep -cE '^[<>].*set default=' <<<"$diffout")
  if (( nchg == 0 )); then
    log "  grub already targets entry $SCAE_ENTRY for ${HOSTS[*]} — no change needed"; rm -f "$orig" "$new"; return 0
  fi
  if (( nchg != nsd || nsd > 8 )); then
    log "  UNSAFE: grub diff not confined to g1-g4 'set default' lines ($nchg changed / $nsd set-default) — REFUSING to push:"
    printf '%s\n' "$diffout" | tee -a "$LOG_FILE"; rm -f "$orig" "$new"; return 1
  fi
  log "  grub diff verified ($nsd 'set default' line(s), g1-g4 only):"; printf '%s\n' "$diffout" | tee -a "$LOG_FILE"

  if ! cat "$new" | ssh -4 -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=10 "$PXE_USER@$PXE_ADDR" \
       "cat > $PXE_GRUB.new-cxlremap-$ts && cp $PXE_GRUB.new-cxlremap-$ts $PXE_GRUB && rm -f $PXE_GRUB.new-cxlremap-$ts"; then
    log "  push failed — restoring backup"; pssh "cp -a $PXE_GRUB.bak.cxlremap-$ts $PXE_GRUB"; rm -f "$orig" "$new"; return 1
  fi
  if ! pssh "cat $PXE_GRUB" | diff - "$new" >/dev/null 2>&1; then
    log "  live grub.cfg mismatch after push — restoring backup"; pssh "cp -a $PXE_GRUB.bak.cxlremap-$ts $PXE_GRUB"; rm -f "$orig" "$new"; return 1
  fi
  log "  grub.cfg updated (${HOSTS[*]} -> SC_AE); backup on $PXE_ADDR: $PXE_GRUB.bak.cxlremap-$ts; other hosts untouched"
  rm -f "$orig" "$new"; return 0
}

cmd_remap() {
  local yes=0 max_retries=2 attempt=1
  while [[ $# -gt 0 ]]; do case "$1" in
    --yes) yes=1; shift ;;
    --max-retries) max_retries=$2; shift 2 ;;
    *) log "unknown arg $1"; exit 2 ;;
  esac; done

  if [[ "$yes" != 1 ]]; then
    read -r -p "This POWERS OFF ${HOSTS[*]} and reboots the H3. Continue? [y/N] " a
    [[ "$a" == y || "$a" == Y ]] || { log "aborted"; exit 1; }
  fi

  # Point the g-hosts' PXE default at the SC_AE (production, debug-OFF) kernel
  # BEFORE any power-cycle, so they boot the right image. Done once; retries reuse
  # it. Set SKIP_GRUB=1 to bypass (e.g. grub already set by hand).
  if [[ "${SKIP_GRUB:-0}" != 1 ]]; then
    pxe_set_scae || { log "ABORT: could not set the PXE SC_AE boot entry on $PXE_ADDR; refusing to power-cycle (hosts would boot their previous default). Fix $PXE_ADDR access or set grub by hand, then re-run (or SKIP_GRUB=1 to bypass)."; exit 6; }
  fi

  while (( attempt <= max_retries + 1 )); do
    log "########## remap cycle attempt $attempt ##########"

    log "step 1: power off all hosts"
    for h in "${HOSTS[@]}"; do host_power "$h" off; done
    for h in "${HOSTS[@]}"; do
      for _ in $(seq 1 15); do [[ "$(host_power_state "$h")" == off ]] && break; sleep 4; done
      log "  $h power: $(host_power_state "$h")"
    done

    log "step 2: reboot H3"
    h3 "reboot" || true
    sleep 15
    local up=0
    for _ in $(seq 1 "$H3_POLL_TRIES"); do h3 true && { up=1; break; }; sleep 8; done
    [[ "$up" == 1 ]] || { log "H3 did not come back — manual attention needed"; exit 3; }
    log "  H3 back online"

    # The /eMMCRS (eMMC) filesystem mounts a few seconds AFTER sshd starts
    # accepting connections; writing the pool map too early fails. Wait until
    # /eMMCRS is mounted and writable before pushing our map.
    local emmc_ready=0
    for _ in $(seq 1 20); do
      [[ "$(h3 "test -d /eMMCRS && test -w /eMMCRS && echo ok")" == ok ]] && { emmc_ready=1; break; }
      sleep 3
    done
    [[ "$emmc_ready" == 1 ]] || { log "H3 /eMMCRS not writable after wait — manual attention"; exit 3; }

    log "step 3: push bundled pool map -> $H3_POOLMAP_SCAE (self-contained; ignores the shared $H3_POOLMAP)"
    [[ -f "$BUNDLED_POOLMAP" ]] || { log "  bundled pool map missing: $BUNDLED_POOLMAP"; exit 4; }
    h3_put "$H3_POOLMAP_SCAE" < "$BUNDLED_POOLMAP" || { log "  failed to push pool map to H3"; exit 4; }
    h3 "chmod +x $H3_POOLMAP_SCAE"
    # confirm it landed intact (line count) and matches the desired mapping
    if [[ "$(h3 "wc -l < $H3_POOLMAP_SCAE" | tr -d ' ')" != "$(wc -l < "$BUNDLED_POOLMAP" | tr -d ' ')" ]]; then
      log "  pushed pool map line-count mismatch — aborting"; exit 4
    fi
    if ! h3_poolmap_verify "$H3_POOLMAP_SCAE"; then
      log "  pushed pool map does not match expected params — aborting"; exit 4
    fi
    log "  pool map pushed + verified OK"

    log "step 4: run remap on H3 (switch power-cycles internally; python errors ignorable)"
    h3 "timeout 300 bash $H3_POOLMAP_SCAE" 2>&1 | tail -5 | tee -a "$LOG_FILE" || true

    log "step 5: power on all hosts"
    for h in "${HOSTS[@]}"; do host_power "$h" on; done

    log "step 6: wait for hosts (PXE+NFS boot) and run prepare.sh"
    local boot_fail=0
    for h in "${HOSTS[@]}"; do
      if wait_host_ssh "$h"; then
        gssh "$h" 'timeout 900 bash -s 2>&1' < "$PREPARE_SH" >>"$LOG_FILE" && log "  $h: prepare OK" || log "  $h: prepare FAILED"
      else
        log "  $h: BOOT TIMEOUT"; boot_fail=$((boot_fail+1))
      fi
    done

    log "step 7: verify H3-side DVSEC (needs hosts POSTed)"
    local dvsec_fail=0
    for h in "${HOSTS[@]}"; do
      local ep=${EP_PORT[$h]} r
      r=$(h3_dvsec_check "$ep")
      log "  $h ep$ep: dvsec=$r link=$(h3_portstatus "$ep")"
      [[ "$r" == "ok $CXL_BASE" ]] || dvsec_fail=$((dvsec_fail+1))
    done

    log "step 8: verify host devices"
    local host_fail=0
    for h in "${HOSTS[@]}"; do host_verify "$h" || host_fail=$((host_fail+$?)); done

    if (( boot_fail == 0 && dvsec_fail == 0 && host_fail == 0 )); then
      log "########## SUCCESS: testbed healthy (log: $LOG_FILE) ##########"
      return 0
    fi
    log "cycle $attempt failed (boot=$boot_fail dvsec=$dvsec_fail host=$host_fail) — retrying"
    attempt=$((attempt+1))
  done

  log "########## STILL UNHEALTHY after $((max_retries+1)) cycles ##########"
  log "Escalation (manual, per H3 runbook): 1) power off all g-hosts  2) power off"
  log "the H3 COMPLETELY (unplug)  3) replug  4) run $H3_POOLMAP  5) power on hosts."
  return 5
}

case "${1:-}" in
  status) shift; cmd_status "$@" ;;
  remap)  shift; cmd_remap  "$@" ;;
  *) echo "usage: $0 {status|remap [--yes] [--max-retries N]}"; exit 2 ;;
esac
