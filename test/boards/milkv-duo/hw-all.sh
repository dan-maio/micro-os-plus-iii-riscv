#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# hw-all.sh -- run the Milk-V Duo's hardware (hwd) tests one after another.
#
#     test/boards/milkv-duo/hw-all.sh                 every built *-hwd test
#     test/boards/milkv-duo/hw-all.sh mos-test1 ...   only these, in this order
#
# Before EVERY test the Duo is warm-reset over JTAG (hw-reset.sh / reset.cfg:
# the SDK's RTC warm-reset sequence) and the script waits GAP seconds
# (default 4, kept within 3..5) for it to boot again from its SD card -- boot
# ROM, minimal fip (BL2: clocks, DDR, UART4, JTAG pinmux), OpenSBI parked.
# Then the test is one hw.sh session: OpenOCD halts the hart, loads the test
# into DDR and runs it to its RESULT line and its semihosting exit. So every
# test starts from a freshly booted SoC, with no peripheral state left over
# from the previous one.
#
# RESET=0 skips the reset and only pauses GAP seconds between tests; that
# works too (every test re-runs _start, which re-initialises what a test
# touches), but the SoC is not otherwise cleaned.
#
# If a test does not end cleanly (TIMEOUT / OPENOCD DIED) the script stops,
# unless KEEP_GOING=1 (with RESET=1 the next reset usually recovers the
# board; if even the reset cannot attach, power-cycle the Duo).
#
# The Duo must have booted from its SD card with the minimal fip.bin once,
# before the first test (see hw.sh).
#
# Env:  BUILD=<dir>       the build directory (default: test/build; under the
#                         harness: tests/build/<config>/platform-bin/port-tests)
#       GAP=<seconds>     reboot wait / pause before each test, 3..5 (default 4)
#       RESET=0|1         warm-reset before each test (default 1)
#       SECS=<seconds>    per-test timeout (default 600)
#       KEEP_GOING=1      carry on after a TIMEOUT / OPENOCD DIED
#       OPENOCD=<path>    a specific OpenOCD (passed to hw.sh)
#
# Output: one line per test, a summary, and the logs in $BUILD/.hw-logs/.
# Exit:   0 if every test passed.
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
export BUILD="${BUILD:-$ROOT/test/build}"

GAP="${GAP:-4}"
if (( GAP < 3 )); then GAP=3; fi
if (( GAP > 5 )); then GAP=5; fi
SECS="${SECS:-600}"
KEEP_GOING="${KEEP_GOING:-0}"
RESET="${RESET:-1}"

if [[ $# -gt 0 ]]; then
  TESTS=("$@")
else
  TESTS=()
  for e in "$BUILD"/test/*-hwd; do
    [[ -e "$e" ]] || continue
    TESTS+=("$(basename "$e" -hwd)")
  done
fi

if [[ ${#TESTS[@]} -eq 0 ]]; then
  echo "hw-all.sh: no *-hwd images in $BUILD/test (build first)" >&2
  exit 2
fi

SUMMARY="$BUILD/.hw-logs/hw-all-$(date +%Y%m%d-%H%M%S).txt"
mkdir -p "$BUILD/.hw-logs"

if [[ "$RESET" == "1" ]]; then
  echo "== Milk-V Duo: ${#TESTS[@]} hwd test(s), warm reset + ${GAP}s boot before each =="
else
  echo "== Milk-V Duo: ${#TESTS[@]} hwd test(s), ${GAP}s between tests (no reset) =="
fi
echo "   build: $BUILD"
echo

pass=0 fail=0 n=0
results=()
start_all=$(date +%s)

for i in "${!TESTS[@]}"; do
  app="${TESTS[$i]}"
  n=$((n + 1))

  if [[ "$RESET" == "1" ]]; then
    if ! bash "$HERE/hw-reset.sh" "$GAP"; then
      results+=("$(printf '%2d/%-2d %-22s %-16s' "$n" "${#TESTS[@]}" "$app" "RESET FAILED")")
      echo "${results[-1]}"
      fail=$((fail + 1))
      echo "hw-all.sh: the reset could not attach; power-cycle the Duo." >&2
      break
    fi
  elif (( i > 0 )); then
    sleep "$GAP"
  fi

  t0=$(date +%s)
  bash "$HERE/hw.sh" "$app" "$SECS"
  rc=$?

  dt=$(( $(date +%s) - t0 ))
  case "$rc" in
    0) verdict="PASS" ; pass=$((pass + 1)) ;;
    1) verdict="FAIL" ; fail=$((fail + 1)) ;;
    2) verdict="MISSING" ; fail=$((fail + 1)) ;;
    3) verdict="TIMEOUT" ; fail=$((fail + 1)) ;;
    *) verdict="OPENOCD DIED" ; fail=$((fail + 1)) ;;
  esac
  results+=("$(printf '%2d/%-2d %-22s %-16s %4ss' "$n" "${#TESTS[@]}" "$app" "$verdict" "$dt")")
  echo "${results[-1]}"

  if [[ ( $rc -eq 3 || $rc -ge 4 ) && "$KEEP_GOING" != "1" ]]; then
    echo
    echo "hw-all.sh: $app did not end cleanly; stopping (KEEP_GOING=1 to continue)."
    echo "           Power-cycle the Duo before re-running."
    break
  fi

done

total=$(( $(date +%s) - start_all ))
{
  echo "Milk-V Duo hwd run $(date '+%Y-%m-%d %H:%M:%S'), reset=${RESET}, gap ${GAP}s, build $BUILD"
  printf '%s\n' "${results[@]}"
  echo "== $pass passed, $fail failed, of $n run (${#TESTS[@]} requested), ${total}s =="
} | tee "$SUMMARY" | tail -1
echo "summary: $SUMMARY"

[[ $fail -eq 0 && $n -eq ${#TESTS[@]} ]]
