#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw.sh -- run one of the Milk-V Duo's tests on the board, over JTAG.
#
#     test/boards/milkv-duo/hw.sh            list the tests
#     test/boards/milkv-duo/hw.sh <app>      load it into DDR and run it
#
# THE BOARD MUST ALREADY BE BOOTED from its SD card with the minimal fip.bin
# (milkv-duo (opensbi)/output/fip.bin): it is what brings up the clocks, the
# DDR, UART4 and the JTAG pinmux, then parks the hart in wfi. Nothing is
# written to the SD card; the test goes into DDR only.
#
# The session, OpenOCD only (no GDB, nothing redirected):
#   init, halt                 stop the parked hart
#   reg priv 3                 the payload ran in S-mode; µOS++ runs in M-mode
#   load_image <elf>           into DDR at 0x80050000 (through the D-cache)
#   arm semihosting enable     the test's output and its SYS_EXIT verdict
#   resume <_start>
#
# There is no reset line on the header: one test per power cycle.
#
# Wiring: TDI pin 1, TDO pin 2, TMS pin 16, TCK pin 17, GND pin 18 (JTAG);
#         UART4 TX pin 4, RX pin 5 (115200 8N1) for the console.
#
# Env:  BUILD=<dir>      the CMake build directory  (default: test/build)
#       OPENOCD=<path>   a specific OpenOCD (one that drives the C906)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build}"

list_tests () {
  echo "tests built for milkv-duo:"
  for e in "$BUILD"/test/*-hwd; do
    [[ -e "$e" ]] || { echo "  (none -- build first)"; return; }
    echo "  $(basename "$e" -hwd)"
  done
}

APP="${1:-}"
if [[ -z "$APP" || "$APP" == "list" ]]; then
  list_tests
  exit 0
fi
SECS="${2:-600}"

ELF="$BUILD/test/${APP}-hwd"
if [[ ! -f "$ELF" ]]; then
  echo "hw.sh: no such test: $APP" >&2
  list_tests >&2
  exit 2
fi

# The entry point, from the ELF header.
NM="$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/riscv-none-elf-gcc/*/.content/bin/riscv-none-elf-readelf 2>/dev/null | sort -V | tail -1)"
NM="${NM:-riscv-none-elf-readelf}"
ENTRY="$("$NM" -h "$ELF" | awk '/Entry point address/ { print $4 }')"

OCD="${OPENOCD:-$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/openocd/*/.content/bin/openocd 2>/dev/null | sort -V | tail -1)}"
OCD="${OCD:-openocd}"
SCRIPTS_ARG=()
if [[ "$OCD" == *"/xPacks/"* ]]; then
  SCRIPTS_ARG=(-s "$(dirname "$(dirname "$OCD")")/openocd/scripts")
fi

LOGS="$BUILD/.hw-logs"; mkdir -p "$LOGS"
LOG="$LOGS/${APP}.log"
rm -f "$LOG"

echo "== Milk-V Duo : $APP (entry $ENTRY) =="

"$OCD" "${SCRIPTS_ARG[@]}" \
  -f "$HERE/openocd.cfg" \
  -c "init" \
  -c "halt" \
  -c "reg priv 3" \
  -c "load_image \"$ELF\" 0 elf" \
  -c "arm semihosting enable" \
  -c "resume $ENTRY" \
  > >(tee "$LOG") 2>&1 &
OCD_PID=$!

waited=0 rc=2
while (( waited < SECS )); do
  if grep -q 'RESULT: PASS' "$LOG" 2>/dev/null; then rc=0; break; fi
  if grep -q 'RESULT: FAIL' "$LOG" 2>/dev/null; then rc=1; break; fi
  if ! kill -0 "$OCD_PID" 2>/dev/null; then rc=4; break; fi
  sleep 1; waited=$((waited + 1))
done

kill "$OCD_PID" 2>/dev/null; wait "$OCD_PID" 2>/dev/null

# OpenOCD's console is block-buffered on a pipe: look once more after exit.
if [[ $rc -eq 2 || $rc -eq 4 ]]; then
  if grep -q 'RESULT: PASS' "$LOG" 2>/dev/null; then rc=0
  elif grep -q 'RESULT: FAIL' "$LOG" 2>/dev/null; then rc=1
  fi
fi

printf '\n%-24s ' "$APP"
case "$rc" in
  0) echo "PASS  (${waited}s)" ;;
  1) echo "FAIL  (see $LOG)" ;;
  4) echo "OPENOCD DIED  (see $LOG)" ;;
  *) echo "TIMEOUT after ${SECS}s  (see $LOG)" ;;
esac
[[ "$rc" -eq 0 ]]
