#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw.sh -- run one of the CH32V307V-EVT-R1 tests on the board.
#
#     test/boards/ch32v307-evt/hw.sh            list the tests
#     test/boards/ch32v307-evt/hw.sh <app>      program it into flash and run it
#
# Probe: the EVT's on-board WCH-LinkE (1a86:8010), RISC-V mode, two-wire
# SDI. OpenOCD: the WCH (MounRiver) fork, mrs-wch-riscv-221010, with one
# patch: its wlink driver knew probe types 1-4 only, and this LinkE
# (firmware 2.10) reports 0x12 -- accepted as type 2, as the newer
# mrs-wch-riscv-230824 wlinke driver does. OpenOCD only, no GDB.
#
# The session:
#   init, reset halt
#   flash write_image erase <elf>
#                              the image is linked at the flash's 0x00000000
#                              alias (where the core starts), and that is
#                              where this OpenOCD's wch_riscv bank is
#                              (flash banks: 0x00000000, measured). The
#                              erase is the whole chip (the WCH driver has no
#                              sector erase).
#   dump_image + cmp           the flash read back and compared with the
#                              image's bytes on the PC (verify_image needs a
#                              work area, and the config's 0x80000000 is not
#                              RAM on this chip)
#   reset halt, arm semihosting enable, resume
#
# Two traps of this OpenOCD (measured):
#   - in WCH mode every target_write_memory below 0x20000000 is treated as
#     flash data and copied into a 256-byte buffer without a bounds check
#     (riscv-013.c write_flash_data): never load_image anything there;
#     flash goes through flash write_image only.
#   - its SYS_CLOCK is OpenOCD's own CPU time (clock()); SYS_TIME is wall
#     time.
#
# Verdict: the RESULT line and the semihosting exit. OpenOCD ends itself on
# SYS_EXIT with the test's exit code. Exit codes as the other boards' hw.sh:
# 0 PASS, 1 FAIL, 2 no such test / stale build, 3 TIMEOUT, 4 OPENOCD DIED,
# 5 NO EXIT, 6 FLASH READBACK DIFFERS.
#
# Env:  BUILD=<dir>      the CMake build directory (default: test/build-ch32v307)
#       OPENOCD=<path>   the WCH OpenOCD (default ~/.local/openocd-wch/bin/openocd)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build-ch32v307}"
BOARD_TESTS="$ROOT/test/ch32v307-evt"

list_tests () {
  echo "tests built for ch32v307-evt:"
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
SECS="${2:-300}"

ELF="$BUILD/test/${APP}-hwd"
[[ -f "$ELF" ]] || ELF="$BUILD/test/${APP}-hwd.elf"
if [[ ! -f "$ELF" ]]; then
  echo "hw.sh: no such test: $APP" >&2
  list_tests >&2
  exit 2
fi
ELF="$(readlink -f "$ELF")"

STALE="$(find "$ROOT/src" "$ROOT/include" "$HERE" \
  "$BOARD_TESTS/$APP" "$BOARD_TESTS/include" "$BOARD_TESTS/src" \
  -type f -newer "$ELF" \( -name '*.cpp' -o -name '*.c' -o -name '*.S' -o -name '*.h' -o -name '*.hpp' -o -name '*.ld' \) 2>/dev/null | head -3)"
if [[ -n "$STALE" ]]; then
  echo "hw.sh: $ELF is older than its sources -- rebuild first:" >&2
  echo "$STALE" | sed 's/^/  /' >&2
  exit 2
fi

OCD="${OPENOCD:-$HOME/.local/openocd-wch/bin/openocd}"
if [[ ! -x "$OCD" ]]; then
  echo "hw.sh: no WCH OpenOCD at $OCD; set OPENOCD" >&2
  exit 4
fi
# The scripts of the installed WCH OpenOCD (interface/wlink.cfg,
# target/wch-riscv.cfg), also when OPENOCD points at a build tree.
SCRIPTS="${OPENOCD_SCRIPTS:-$HOME/.local/openocd-wch/share/openocd/scripts}"

TOOLBIN="$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/riscv-none-elf-gcc/*/.content/bin 2>/dev/null | sort -V | tail -1)"
OBJCOPY="${TOOLBIN:+$TOOLBIN/}riscv-none-elf-objcopy"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
# The bytes that go to flash: every loadable section at its load address
# (.text .. .eh_frame, then .data's initial values), from 0x00000000.
"$OBJCOPY" -O binary --only-section=.text --only-section=.rodata \
  --only-section=.preinit_array --only-section=.init_array \
  --only-section=.fini_array --only-section=.eh_frame --only-section=.data \
  "$ELF" "$WORK/image.bin" || exit 2
SIZE="$(stat -c %s "$WORK/image.bin")"

echo "hw.sh: $APP -> flash ($SIZE bytes), OpenOCD $OCD"

# 1. program and read back (no run yet)
"$OCD" -s "$SCRIPTS" -f interface/wlink.cfg -f target/wch-riscv.cfg \
  -c "init" -c "reset halt" \
  -c "flash write_image erase \"$ELF\"" \
  -c "dump_image \"$WORK/back.bin\" 0x00000000 $SIZE" \
  -c "shutdown" > "$WORK/program.log" 2>&1
rc=$?
if [[ $rc -ne 0 ]]; then
  cat "$WORK/program.log" >&2
  echo "hw.sh: OPENOCD DIED while programming (rc=$rc)" >&2
  exit 4
fi
if ! cmp -s "$WORK/image.bin" "$WORK/back.bin"; then
  cmp "$WORK/image.bin" "$WORK/back.bin" | head -3 >&2
  echo "hw.sh: FLASH READBACK DIFFERS" >&2
  exit 6
fi
echo "hw.sh: flash programmed and read back identical"

# 2. run from reset with semihosting; OpenOCD ends on the test's SYS_EXIT
timeout "$SECS" "$OCD" -s "$SCRIPTS" -f interface/wlink.cfg -f target/wch-riscv.cfg \
  -c "init" -c "reset halt" -c "arm semihosting enable" -c "resume" \
  2>&1 | tee "$WORK/run.log" | grep -v "^Info \|^Warn \|^Open On-Chip\|^Licensed\|^For bug\|^\s*http\|^\[wch-rv\|^$\|semihosting is enabled"
rc=${PIPESTATUS[0]}

case $rc in
  0) echo "hw.sh: $APP PASS"; exit 0 ;;
  1) echo "hw.sh: $APP FAIL"; exit 1 ;;
  124) echo "hw.sh: $APP TIMEOUT after ${SECS}s"; exit 3 ;;
  *)
    if grep -q "RESULT:" "$WORK/run.log"; then
      echo "hw.sh: $APP NO EXIT (rc=$rc)"; exit 5
    fi
    echo "hw.sh: $APP OPENOCD DIED (rc=$rc)"; exit 4 ;;
esac
