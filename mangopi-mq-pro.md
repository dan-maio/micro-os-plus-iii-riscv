# MangoPi MQ-Pro (Allwinner D1) — µOS++ III RISC-V port

**Board:** MangoPi MQ-Pro, Allwinner D1 (sun20i), one T-Head C906 (RV64GC), 512 MiB or 1 GiB DDR3  
**Loader:** xfel over the USB-C **OTG** FEL port; image at `0x40000000`  
**Console:** UART0, PB8 = TX / PB9 = RX, 115200 8N1 — left entirely to the user  
**Port id:** `mangopi-mq-pro` (`-DBOARD=mangopi-mq-pro`)  
**Result:** hardware **10/10 port + 6/6 harness PASS** (xfel only); QEMU **11/11 port + 6/6 harness PASS**

---

## 1. Summary

The D1 C906 is driven in machine mode from DDR. There is no JTAG probe on this
setup, so the port does not use OpenOCD or semihosting on hardware:

- **xfel** initializes DDR, writes the flat image at `0x40000000` and jumps to it.
- The test console is UART0; `hw.sh` never opens or configures the serial port.
- The **verdict** is exchanged through the battery-backed RTC general-purpose
  registers, which survive the reset and are readable over FEL.
- Every test ends by resetting the SoC back to FEL, on PASS, FAIL, a returning
  `os_main`, or a trap.
- QEMU's `virt` machine is used for the emulated images (QEMU does not model the
  D1), exactly as the Milk-V Duo port does.

---

## 2. Hardware facts used

| Item | Value |
|---|---|
| CPU | T-Head C906, RV64GC, mhartid 0 |
| DDR | `0x40000000`; the port maps and uses the first 512 MiB |
| Timebase | 24 MHz, read with `rdtime` (`time` CSR) |
| CLINT | `0x14000000`: MSIP `+0x0`, MTIMECMP0 `+0x4000` |
| CLINT MTIME | **not implemented**; `MTIME_READABLE = false`, time from `rdtime` |
| MTIMECMP access | 32-bit halves (`MTIMECMP_SPLIT32 = true`) |
| PLIC | `0x10000000` |
| UART0 | `0x02500000`; PB8/PB9 mux 6; APB1 24 MHz; divisor 13 → 115384 Bd |
| LED | PD18, active high (onboard blue status LED) |
| MMU | Sv39, 2 MiB leaves; C906 `mxstatus.MAEE` set by the board |
| Reset to FEL | RTC GP_DATA2 `0x5AA5A55A` + watchdog soft reset `0x020500A8 = 0x16AA0001` |

The MMU uses T-Head extended PTE attributes (RAM `C|B|SH`, I/O `SO|SH`), so
`riscv_board_initialize_early()` sets `mxstatus.MAEE` before `mmu::init()`. The
board also enables the C906 I/D caches (MHCR bits 0 and 1).

---

## 3. What was added

```
test/boards/mangopi-mq-pro/
  board.cmake                 board facts discovered by CMake
  linker.ld                   image at 0x40000000, 512 MiB window
  include/board.hpp           UART baud, scratch area, nominal CPU clock
  include/riscv-board.hpp     CLINT/PLIC/timebase/MMU regions/hooks
  include/uart.hpp            UART0 API + test banner strings
  include/led.hpp             PD18 API
  include/smp.hpp             RAM window
  src/board.cpp               MAEE, caches, UART init, RTC verdict + reset
  src/uart.cpp                D1 UART0 (clock, pinmux, divisor)
  src/led.cpp                 PD18 GPIO
  hw.sh                       xfel-only one-test runner (no UART access)
  hw-all.sh                   batch runner (still xfel-only)
  qemu.sh                     QEMU-virt runner
  README.md

test/mangopi-mq-pro/
  include/hw_result.hpp       hardware verdict/reset override
  include/harness-suite.hpp   harness-suite verdict wrapper
  tests.cmake                 QEMU stand-in, harness suites, QEMU-only test
  clock-catchup/ clock-test/ exc-test/ mmu-test/ mos-test1/
  mutex-ceiling-test/ sc-test-ko/ smp-mat-test/ smp_test0/ smp-test1/
  semihosting-test/           QEMU only (no debugger on the board)
  blinky/ cmsis-os-validator/ fp-switch/ instrumentation/
  mutex-stress/ rtos-apis/    harness-suite placeholders
  README.md
```

Shared port changes, all backward compatible for the other boards:

- `src/semihosting-exit.cpp` — weak `riscv_board_exit(int)` hook in `_Exit()`.
- `src/trap.cpp` — the fault path calls the same weak hook before the
  semihosting report.
- `test/CMakeLists.txt` — new `BOARD_TEST_QEMU_ONLY` list.

The board defines `riscv_board_exit()` and `riscv_board_test_finish()`, which
write the RTC verdict and reset to FEL.

---

## 4. Build

```sh
cmake -S . -B test/build-mangopi-mq-pro -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/riscv-none-elf.cmake \
      -DBOARD=mangopi-mq-pro
cmake --build test/build-mangopi-mq-pro -j
```

Toolchain: xPack `riscv-none-elf-gcc 15.2.0-1.1`, flags
`-march=rv64imafdc_zicsr_zifencei -mabi=lp64d -mcmodel=medany -mtune=thead-c906`.

---

## 5. QEMU

The D1 is not modelled by QEMU, so the `qemu` image of each test links the
generic `micro-os-plus::riscv-qemu-virt` board (virt CLINT, NS16550 console,
semihosting, its own linker script). The test sources are unchanged.

```sh
(cd test/build-mangopi-mq-pro && ctest -LE hwd)
# one test:
BOARD=mangopi-mq-pro BUILD=test/build-mangopi-mq-pro test/qemu.sh clock-test
```

All eleven QEMU cases pass.

---

## 6. Hardware runner (xfel only)

`hw.sh` never opens, configures or reads the serial port. The test console goes
to UART0 for you to watch with your own terminal.

```sh
BUILD=test/build-mangopi-mq-pro \
  test/boards/mangopi-mq-pro/hw.sh <test>
```

The board must be in FEL mode: unplug the USB-C OTG cable, remove the microSD
card, plug it back in. With no boot media the BROM falls back to FEL **after
each reset**, which is what makes test-after-test runs possible.

Sequence:

```text
xfel reset                         # clean FEL
clear RTC GP_DATA3/4
xfel ddr d1
xfel write 0x40000000 <app>.bin    # objcopy from the ELF
xfel exec 0x40000000
wait for FEL (the test resets itself)
read RTC GP_DATA3/4 over xfel
xfel reset                         # clean FEL for the next test
```

`hw-all.sh` runs the whole set hands-off, but the normal flow is one test at a
time, waiting for a human confirmation between tests.

---

## 7. Verdict and reset protocol

RTC general-purpose registers survive the watchdog reset and are readable over
FEL:

| Register | Address | Value |
|---|---|---|
| GP_DATA2 | `0x07090108` | `0x5AA5A55A` — Allwinner `efex` request |
| GP_DATA3 | `0x0709010C` | `0x4D515052` (`MQPR`) — valid verdict |
| GP_DATA4 | `0x07090110` | `0` PASS, `1` FAIL, `2` SKIP |

Every end path calls `riscv_board_test_finish()`:

- the tests' `hw_result::ok()` / `fail()`;
- `_Exit()` when `os_main` returns, through the weak `riscv_board_exit()` hook;
- the trap handler for an unhandled exception (through the same hook).

`riscv_board_test_finish()` writes the RTC verdict, then
`riscv_board_reset_to_fel()` writes the `efex` magic and the D1 watchdog
soft-reset (`0x020500A8 = 0x16AA0001`; a 1 s interval watchdog is armed as a
fallback). The runner additionally issues an explicit `xfel reset` before and
after each test.

The UART is purely for the user: the hardware verdict does not depend on it.

---

## 8. Results

### Hardware (xfel only) — 10/10 PASS

| # | Test | Result |
|---|---|---|
| 1 | clock-catchup | PASS |
| 2 | clock-test | PASS |
| 3 | exc-test | PASS |
| 4 | mmu-test | PASS |
| 5 | mos-test1 | PASS |
| 6 | mutex-ceiling-test | PASS |
| 7 | sc-test-ko | PASS |
| 8 | smp-mat-test (N=320/B=40) | PASS |
| 9 | smp_test0 | PASS |
| 10 | smp-test1 | PASS |

### QEMU virt — 11/11 PASS

The same ten applications plus `semihosting-test`, which is QEMU-only because
the hardware setup has no debugger to service semihosting.

### Harness suites — 6/6 QEMU, 6/6 hardware

Built by the kernel harness platform `tests/platforms/riscv-mangopi-mq-pro`
(see section 10), and run on hardware through the same xfel-only `hw.sh`.

| Suite | QEMU | Hardware (xfel) |
|---|---|---|
| blinky | PASS | PASS |
| fp-switch | PASS | PASS |
| mutex-stress | PASS | PASS (38.6 s) |
| instrumentation | PASS | PASS |
| rtos-apis | PASS | PASS |
| cmsis-os-validator | PASS | PASS |

---

## 9. Test adaptations

- **`mmu-test`**: the unmapped hole and alias VA is `0x20000000`. On the Duo
  the test used `0x40000000`, but on the D1 that address is DDR.
- **`semihosting-test`**: listed in `BOARD_TEST_QEMU_ONLY`; no hardware image.
- **`hw_result.hpp`**: shadows the kernel's header. On `QEMU_BUILD` it uses
  semihosting; on hardware it prints the RESULT line to UART, writes the RTC
  verdict and resets.
- **`smp-mat-test`**: N/B are `SMP_MAT_N` / `SMP_MAT_B`; this board's
  `tests.cmake` builds it with N=320, B=40 (8 row blocks, single hart), and
  the hardware run at that size passed.
- The test sources are the portable RISC-V set of the Milk-V Duo; the board
  banner comes from `test/boards/mangopi-mq-pro/include/uart.hpp`.

---

## 10. Harness suites

The six suites (`blinky`, `fp-switch`, `mutex-stress`, `instrumentation`,
`rtos-apis`, `cmsis-os-validator`) are built by the kernel harness. Added:

```text
micro-os-plus-iii/tests/platforms/riscv-mangopi-mq-pro/
  CMakeLists.txt
  cmake/{definitions,platform-library,dependencies-folders}.cmake
  include/cmsis-plus/platform.h
  include/blink-led.h

micro-os-plus-iii/tests/package.json
  riscv-mangopi-mq-pro-cmake-gcc-debug
  riscv-mangopi-mq-pro-cmake-gcc-release
```

The platform honours the port's `BOARD_TEST_QEMU_ONLY` / `BOARD_TEST_HWD_ONLY`
lists, so `semihosting-test` is registered for QEMU only. Hardware runs use the
port's xfel-only `hw.sh` and the RTC verdict; UART is never opened.

```sh
cd micro-os-plus-iii/tests
xpm run install --config riscv-mangopi-mq-pro-cmake-gcc-debug
xpm run prepare --config riscv-mangopi-mq-pro-cmake-gcc-debug
xpm run build   --config riscv-mangopi-mq-pro-cmake-gcc-debug

# all QEMU cases (port tests + harness suites)
xpm run test --config riscv-mangopi-mq-pro-cmake-gcc-debug

# one hardware harness suite at a time
xpm run test-blinky-hwd --config riscv-mangopi-mq-pro-cmake-gcc-debug
```

Not ported: `smp-test-nested` (CV1800B PLIC/DW timers/UART4) and the TinyUSB
CDC/HID tests. The D1 has a different timer/PLIC layout and this port has no
USB device stack yet.

---

## 11. Quick reference

```sh
# build
cmake -S . -B test/build-mangopi-mq-pro -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/riscv-none-elf.cmake \
      -DBOARD=mangopi-mq-pro
cmake --build test/build-mangopi-mq-pro -j

# QEMU, all
(cd test/build-mangopi-mq-pro && ctest -LE hwd)

# hardware, one test (UART untouched)
BUILD=test/build-mangopi-mq-pro test/boards/mangopi-mq-pro/hw.sh smp_test0

# hardware, list
BUILD=test/build-mangopi-mq-pro test/boards/mangopi-mq-pro/hw.sh list

# harness suites (kernel tests/ tree)
cd micro-os-plus-iii/tests
xpm run build --config riscv-mangopi-mq-pro-cmake-gcc-debug
xpm run test --config riscv-mangopi-mq-pro-cmake-gcc-debug
xpm run test-blinky-hwd --config riscv-mangopi-mq-pro-cmake-gcc-debug
```
