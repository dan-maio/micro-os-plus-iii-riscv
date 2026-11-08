# MangoPi MQ-Pro tests

The portable RISC-V test set, adapted from the Milk-V Duo for the Allwinner
D1 (one T-Head C906, 512 MiB/1 GiB DDR at `0x40000000`). Each directory is
one application with its own `os_main`; the board's `hw.sh` runner loads it
with xfel, and the verdict comes back through the RTC registers. The UART
is never opened by the runner.

## Hardware tests (xfel + RTC verdict)

`clock-catchup`, `clock-test`, `exc-test`, `mmu-test`, `mos-test1`,
`mutex-ceiling-test`, `sc-test-ko`, `smp-mat-test`, `smp_test0`,
`smp-test1`.

All ten pass on the MQ-Pro.

## QEMU tests

The same applications are also built for QEMU's virt machine (the D1 is not
modelled); `semihosting-test` exists only there, because hardware has no
debugger. All eleven QEMU cases pass.

## Build

```sh
cmake -S . -B test/build-mangopi-mq-pro -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/riscv-none-elf.cmake \
      -DBOARD=mangopi-mq-pro
cmake --build test/build-mangopi-mq-pro -j
```

## Run

QEMU:

```sh
(cd test/build-mangopi-mq-pro && ctest -LE hwd)
# or one:
BOARD=mangopi-mq-pro BUILD=test/build-mangopi-mq-pro test/qemu.sh clock-test
```

Hardware, one at a time:

```sh
BUILD=test/build-mangopi-mq-pro \
  test/boards/mangopi-mq-pro/hw.sh smp_test0
```

The board must be in FEL with no microSD card. The runner resets the SoC
before and after the test; the test itself also resets at PASS/FAIL/return/
trap. `hw-all.sh` runs the whole set hands-off, but the normal flow is one
test at a time, with the UART free for your terminal.

## Board-specific adaptations

- `mmu-test`: the unmapped hole/alias VA is `0x20000000` (on the Duo it was
  `0x40000000`, which is DDR on the D1).
- `semihosting-test`: QEMU-only (`BOARD_TEST_QEMU_ONLY`).
- `hw_result.hpp` (in `test/mangopi-mq-pro/include/`) shadows the kernel's:
  on hardware it prints the RESULT line to UART, writes the RTC verdict and
  resets; under QEMU it uses semihosting.
- `smp-mat-test`: N/B are `SMP_MAT_N` / `SMP_MAT_B`; the MQ-Pro build uses
  N=320, B=40 (8 row blocks, single hart) and passes on hardware.
- The six harness-suite directories (`blinky`, `fp-switch`,
  `mutex-stress`, `instrumentation`, `rtos-apis`, `cmsis-os-validator`) are
  built by the kernel harness platform
  `micro-os-plus-iii/tests/platforms/riscv-mangopi-mq-pro/`.

## Harness suites

All six harness suites pass on QEMU and on hardware (xfel only, UART
untouched):

| Suite | QEMU | Hardware |
|---|---|---|
| blinky | PASS | PASS |
| fp-switch | PASS | PASS |
| mutex-stress | PASS | PASS |
| instrumentation | PASS | PASS |
| rtos-apis | PASS | PASS |
| cmsis-os-validator | PASS | PASS |

```sh
cd micro-os-plus-iii/tests
xpm run install --config riscv-mangopi-mq-pro-cmake-gcc-debug
xpm run prepare --config riscv-mangopi-mq-pro-cmake-gcc-debug
xpm run build   --config riscv-mangopi-mq-pro-cmake-gcc-debug
xpm run test    --config riscv-mangopi-mq-pro-cmake-gcc-debug
xpm run test-blinky-hwd --config riscv-mangopi-mq-pro-cmake-gcc-debug
```

## Not ported

`smp-test-nested` (CV1800B PLIC/DW timers/UART4) and the TinyUSB CDC/HID
tests are Milk-V Duo hardware tests; the D1 has different timers/PLIC and
this port has no USB device stack yet.
