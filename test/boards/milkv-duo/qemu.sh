#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# qemu.sh -- run the Milk-V Duo's tests emulated.
#
#     test/boards/milkv-duo/qemu.sh            run every test, one at a time
#     test/boards/milkv-duo/qemu.sh <app>      run one
#
# QEMU does not model the CV1800B: the `qemu` image of each test is built
# against QEMU's virt machine (tests.cmake), RV64GC, 128 MiB, no firmware --
# the image is the reset vector. The console is UART0 on stdio, semihosting
# is on (the verdict ends the run through SYS_EXIT).
#
# Verdict: the exit code AND the RESULT line must agree.
#
# Env:  BUILD=<dir>   the CMake build directory   (default: test/build)
#       QEMU=<path>   a specific qemu-system-riscv64
#       SECS=<n>      per-test timeout             (default: 120)
# -----------------------------------------------------------------------------
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BUILD="${BUILD:-$ROOT/test/build}"
SECS="${SECS:-120}"

Q="${QEMU:-$(ls -d "$HOME"/.local/xPacks/@xpack-dev-tools/qemu-riscv/*/.content/bin/qemu-system-riscv64 2>/dev/null | sort -V | tail -1)}"
Q="${Q:-qemu-system-riscv64}"

LOGS="$BUILD/.qemu-logs"; mkdir -p "$LOGS"

run_one () {
  local app="$1"
  # The harness build names images *.elf; the port's own build does not.
  local elf="$BUILD/test/${app}-qemu.elf"
  [[ -f "$elf" ]] || elf="$BUILD/test/${app}-qemu"
  if [[ ! -f "$elf" ]]; then
    echo "qemu.sh: no such image: $elf (build first)" >&2
    return 2
  fi
  local log="$LOGS/${app}.log"
  echo "== QEMU virt (milkv-duo tests) : $app =="
  timeout "$SECS" "$Q" -M virt -cpu rv64 -smp 1 -m 128M -nographic \
    -bios none -kernel "$elf" \
    -semihosting-config enable=on,target=native \
    -monitor none -serial stdio 2>&1 | tee "$log"
  local rc=${PIPESTATUS[0]}
  if [[ $rc -eq 124 ]]; then
    echo "$app: TIMEOUT after ${SECS}s"
    return 3
  fi
  if [[ $rc -eq 0 ]] && grep -q 'RESULT: PASS' "$log"; then
    echo "$app: PASS"
    return 0
  fi
  echo "$app: FAIL (exit $rc, see $log)"
  return 1
}

if [[ $# -ge 1 ]]; then
  run_one "$1"
  exit $?
fi

fails=0 n=0
for elf in "$BUILD"/test/*-qemu.elf "$BUILD"/test/*-qemu; do
  [[ -e "$elf" ]] || continue
  app="$(basename "$(basename "$elf" .elf)" -qemu)"
  n=$((n + 1))
  run_one "$app" || fails=$((fails + 1))
done
[[ $n -gt 0 ]] || { echo "no *-qemu images in $BUILD/test (build first)" >&2; exit 2; }
echo "== $((n - fails))/$n passed =="
[[ $fails -eq 0 ]]
