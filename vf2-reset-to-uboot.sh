#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# vf2-reset-to-uboot.sh -- reset the VisionFive 2 Lite and hold a key down so
# the U-Boot autoboot countdown is interrupted (no Linux boot).
#
#     ./vf2-reset-to-uboot.sh [serial-device] [seconds]
#
# Defaults:
#   serial   /dev/ttyUSB0
#   seconds  60 with RESET=wdt, else 15
#
# Environment:
#   RESET    wdt | srst | power | halt | run   (default: wdt)
#   CFG      OpenOCD config (default: by RESET, from PROJ)
#   PROJ     directory holding the jh7110_*.cfg files (default: this one)
#   OPENOCD  path to openocd
#   BAUD     115200
#   KEY      "\r" (CR; any key stops U-Boot autoboot)
#
# IMPORTANT, measured on the VisionFive 2 Lite v1.1:
#
#   The RISC-V Debug Module's `reset halt` / `reset run` do NOT reboot the
#   SoC. They halt/resume the U74 harts and leave the loaded image in place.
#   A real board reset needs one of:
#
#     RESET=wdt    (default) the JH7110 watchdog, armed over JTAG with
#                  jh7110_wdt_reset.cfg, disarmed after the reboot with
#                  jh7110_wdt_disable.cfg. No wire needed.
#
#     RESET=srst   the probe's RST/SRST wired to 40-pin header pin 36
#                  (GND to pin 39). Uses jh7110_daplink_srst.cfg.
#
#     RESET=power  no wire: this script just sends the key stream; you
#                  power-cycle the board (or press an external reset button).
#
#     RESET=halt   only for testing: reset halt + resume through the RISC-V
#     RESET=run    DM; on this board it does not restart U-Boot.
#
# The key stream starts BEFORE the reset and continues for the whole boot
# (boot ROM -> SPL -> OpenSBI -> U-Boot). U-Boot's countdown is 3 s; a CR
# every 50 ms is plenty.
# -----------------------------------------------------------------------------
set -uo pipefail

SERIAL="${1:-/dev/ttyUSB0}"
RESET="${RESET:-wdt}"
SECS="${2:-}"
if [[ -z "$SECS" ]]; then
  if [[ "$RESET" == wdt ]]; then SECS=60; else SECS=15; fi
fi
BAUD="${BAUD:-115200}"
KEY="${KEY:-\r}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="${PROJ:-$HERE}"   # the cfgs sit beside this script (repo root)
CFG_MAIN="${CFG_MAIN:-$PROJ/jh7110_daplink.cfg}"
CFG_SRST="${CFG_SRST:-$PROJ/jh7110_daplink_srst.cfg}"
CFG_WDT="${CFG_WDT:-$PROJ/jh7110_wdt_reset.cfg}"
CFG_WDT_OFF="${CFG_WDT_OFF:-$PROJ/jh7110_wdt_disable.cfg}"
CFG="${CFG:-}"

case "$RESET" in
  wdt)       [[ -n "$CFG" ]] || CFG="$CFG_WDT" ;;
  srst|run)  [[ -n "$CFG" ]] || CFG="$CFG_SRST" ;;
  halt)      [[ -n "$CFG" ]] || CFG="$CFG_MAIN" ;;
  power|none) ;;
  *) echo "vf2-reset-to-uboot: RESET must be wdt, srst, power, halt or run" >&2; exit 2 ;;
esac

OCD="${OPENOCD:-$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/openocd/*/.content/bin/openocd 2>/dev/null | sort -V | tail -1)}"
OCD="${OCD:-openocd}"
SCRIPTS_ARG=()
if [[ "$OCD" == *"/xPacks/"* ]]; then
  SCRIPTS_ARG=(-s "$(dirname "$(dirname "$OCD")")/openocd/scripts")
fi

die () { echo "vf2-reset-to-uboot: $*" >&2; exit 1; }

[[ -e "$SERIAL" ]] || die "no such serial device: $SERIAL"
[[ "$SECS" =~ ^[0-9]+$ ]] || die "seconds must be a number"
if [[ "$RESET" != power && "$RESET" != none ]]; then
  [[ -f "$CFG" ]] || die "no such OpenOCD config: $CFG"
fi

if command -v fuser >/dev/null 2>&1; then
  HOLDERS="$(fuser "$SERIAL" 2>/dev/null || true)"
  if [[ -n "${HOLDERS// /}" ]]; then
    echo "note: $SERIAL is also open by PID(s):$HOLDERS"
    echo "      (the key sender can still write; the other program shows output)"
  fi
fi

stty -F "$SERIAL" "$BAUD" raw -echo -echoe -echok -echoctl -echoke \
  clocal -crtscts 2>/dev/null \
  || die "cannot configure $SERIAL (permissions? close the terminal?)"

echo "reset-to-uboot: sending keys on $SERIAL for ${SECS}s"
(
  end=$((SECONDS + SECS))
  while (( SECONDS < end )); do
    printf '%b' "$KEY" > "$SERIAL" 2>/dev/null || break
    sleep 0.05
  done
) &
KEY_PID=$!

sleep 0.2

case "$RESET" in
  wdt)
    echo "reset-to-uboot: watchdog reset (no reset pin needed)"
    echo "                cfg $CFG"
    "$OCD" "${SCRIPTS_ARG[@]}" -f "$CFG"
    rc=$?
    if (( rc == 0 )); then
      # The WDT is in the always-on domain; after the reset it may still be
      # running. Wait past the first timeout, then disarm it once U-Boot/JTAG
      # is back. Retry because the board may still be booting.
      echo "reset-to-uboot: waiting for the reset, then disarming the WDT..."
      sleep 25
      for _try in $(seq 1 15); do
        if "$OCD" "${SCRIPTS_ARG[@]}" -f "$CFG_WDT_OFF" >/dev/null 2>&1; then
          echo "reset-to-uboot: WDT disabled"
          break
        fi
        sleep 2
      done
    fi
    ;;
  srst|run)
    echo "reset-to-uboot: SRST reset (RST must be wired to pin 36)"
    echo "                cfg $CFG"
    "$OCD" "${SCRIPTS_ARG[@]}" -f "$CFG" \
      -c 'init' \
      -c 'reset run' \
      -c 'shutdown'
    rc=$?
    ;;
  halt)
    echo "reset-to-uboot: reset halt + resume through the RISC-V DM"
    echo "                (does not reboot the SoC on this board)"
    "$OCD" "${SCRIPTS_ARG[@]}" -f "$CFG" \
      -c 'init' \
      -c 'reset halt' \
      -c 'resume' \
      -c 'shutdown'
    rc=$?
    ;;
  power|none)
    echo "reset-to-uboot: no OpenOCD reset -- power-cycle / press RESET now."
    echo "                keys are already flowing; you have ${SECS}s."
    rc=0
    ;;
esac

wait "$KEY_PID" 2>/dev/null || true

if (( rc != 0 )); then
  echo "vf2-reset-to-uboot: OpenOCD returned $rc" >&2
  exit "$rc"
fi

echo
echo "done. Check the serial terminal for the 'StarFive #' prompt."
if [[ "$RESET" == wdt ]]; then
  echo "If the board kept resetting: run"
  echo "  openocd -f $CFG_WDT_OFF"
  echo "or power-cycle it."
elif [[ "$RESET" == srst || "$RESET" == run ]]; then
  echo "If Linux still booted: verify RST/SRST is wired to pin 36, or use"
  echo "  RESET=power $0 $SERIAL $SECS"
fi
