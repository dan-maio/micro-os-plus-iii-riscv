#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw-reset.sh -- warm-reset the Milk-V Duo over JTAG and wait for it to boot.
#
#     test/boards/milkv-duo/hw-reset.sh [wait-seconds]
#
# OpenOCD runs reset.cfg (the SDK's RTC warm-reset sequence), then this
# script waits for the reboot through the boot ROM, the SD card's minimal fip
# (BL2: clocks, DDR, UART4, JTAG pinmux) and OpenSBI -- default 4 s, kept
# within 3..5. hw-all.sh calls it between tests.
#
# Exit: 0 if OpenOCD reached the reset request, 1 if it could not attach.
#
# Env:  OPENOCD=<path>   a specific OpenOCD
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WAIT="${1:-4}"
if (( WAIT < 3 )); then WAIT=3; fi
if (( WAIT > 5 )); then WAIT=5; fi

OCD="${OPENOCD:-$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/openocd/*/.content/bin/openocd 2>/dev/null | sort -V | tail -1)}"
OCD="${OCD:-openocd}"
SCRIPTS_ARG=()
if [[ "$OCD" == *"/xPacks/"* ]]; then
  SCRIPTS_ARG=(-s "$(dirname "$(dirname "$OCD")")/openocd/scripts")
fi

out="$("$OCD" "${SCRIPTS_ARG[@]}" -f "$HERE/openocd.cfg" -f "$HERE/reset.cfg" 2>&1)"
if grep -q "warm reset requested" <<<"$out"; then
  echo "reset: requested; waiting ${WAIT}s for the Duo to boot"
  sleep "$WAIT"
  exit 0
fi

echo "reset: OpenOCD did not reach the reset request:" >&2
tail -5 <<<"$out" >&2
exit 1
