# micro-os-plus-iii-riscv

The **RISC-V port of µOS++ III**: RV64GC, machine mode, one hart. The fourth
architecture project beside `micro-os-plus-iii-cortexm`, `-aarch32` and
`-aarch64`, built and tested the same way.

First board: **Milk-V Duo (64 MB)** -- Sophgo CV1800B, T-Head C906 @ 850 MHz,
64 MiB DDR2.

## Layout

```text
micro-os-plu-iii-smp-riscv-pr-github/       the workspace
├── micro-os-plus-iii/                      the kernel (sibling clone)
└── micro-os-plus-iii-riscv/                this port
    ├── CMakeLists.txt                      micro-os-plus::riscv, ::riscv-bare,
    │                                       ::riscv-qemu-virt
    ├── cmake/toolchains/riscv-none-elf.cmake
    ├── include/                            the port contract + ISA headers
    │   ├── cmsis-plus/rtos/port/           os-c-decls.h, os-decls.h, os-inlines.h
    │   ├── port_ctx.hpp                    the trap frame (528 bytes)
    │   ├── riscv-csr.hpp, clint.hpp, mmu.hpp, semihosting.hpp
    │   └── cmsis_device.h, riscv-eabi-compat.h
    ├── src/
    │   ├── startup.S, startup.cpp          reset entry, start-up hooks, init arrays
    │   ├── trap.S, trap.cpp                the single trap entry and dispatcher
    │   ├── rtos/os-core.cpp                the scheduler's port half
    │   ├── soft-irq.cpp                    software IRQ lines (the "NVIC")
    │   ├── mmu.cpp                         Sv39 identity map
    │   ├── trace.cpp, syscalls.c, syscalls-time.cpp, semihosting-exit.cpp
    ├── qemu/virt/                          the QEMU `virt` stand-in board
    ├── scripts/make-pdf.sh                 Markdown -> PDF (pandoc + weasyprint)
    └── test/
        ├── CMakeLists.txt                  the shared test builder (as cortexm/aarch*)
        ├── qemu.sh, hw.sh, hw-all.sh       dispatchers -> boards/<board>/
        ├── boards/milkv-duo/               board.cmake, linker.ld, openocd.cfg,
        │                                   qemu.sh, hw.sh, hw-all.sh,
        │                                   hw-reset.sh + reset.cfg,
        │                                   wait-power-cycle.sh, include/, src/
        └── milkv-duo/                      one directory per test application
```

## The port

RISC-V has no PendSV, and the port does not imitate one. The context switch
is built on what the RISC-V privileged spec guarantees:

| situation | Cortex-M (upstream) | this port |
|---|---|---|
| a thread blocks / yields | set PendSV pending | **`ecall`** (a7 = `RISCV_PORT_YIELD_ECALL`): a precise, synchronous exception, taken AT the instruction; `trap.cpp` calls `switch_stacks()` and `mret`s on the incoming thread's frame |
| an interrupt makes a thread due (tick, post from an ISR) | ISR sets PendSV, tail-chained | `is_reschedule_pending` only; the switch is made **on the way out of the outermost trap** |
| switch asked for inside a critical section | PendSV waits for PRIMASK | deferred; `critical_section::exit()` yields when it sets `mstatus.MIE` again |
| scheduler locked | nothing until unlock | same; unlocking yields if a switch is pending |
| SysTick | SysTick | machine timer (CLINT `mtimecmp` against `rdtime`) |
| PRIMASK | PRIMASK | `mstatus.MIE` (`csrrci`/`csrrsi`, atomic) |
| exception frame + r4-r11 | | full frame by `trap.S`: x1..x31, mepc, mstatus, fcsr, f0..f31 (528 bytes) |
| IPSR != 0 | | `riscv_trap_nesting != 0` |
| NVIC (validator's IRQ 0) | | software IRQ lines multiplexed on CLINT MSIP (`soft-irq.cpp`) |

**Why not MSIP as "PendSV"** (the first version did that): a CLINT MSIP
write is a posted store. QEMU takes the interrupt on the next instruction;
the C906 takes it some cycles later. The kernel marks a thread suspended,
calls `reschedule()` and, once that returns, removes the thread's wake-up
node. On the Duo the thread ran past `reschedule()` before the switch, removed
its own wake-up, and was then switched out for ever. Measured with GDB on the
board: every thread `suspended`, `clock_node_ = 0`, in no list; `smp_test0`
stopped after its first `sleep_for`. With `ecall` there is no such window.
For the same reason `NVIC_SetPendingIRQ()` from a thread (interrupts
enabled) waits until the line's handler has run, as it has on Cortex-M.

The port's `ecall` marker in `a7` is consumed by the trap handler (`a7`
comes back 0), so an application's own `ecall` (`exc-test`) is never taken
for a yield. Idle never executes `wfi` (`wait_for_interrupt()` is empty).

The tick is re-armed on a fixed grid of `rdtime` deadlines.

Semihosting is the `slli/ebreak/srai` sequence; without a debugger the trap
handler skips it (a0 = -1) instead of faulting. The kernel's "embedded" code
paths are selected with `__ARM_EABI__`, as on AArch64 (see
`include/riscv-eabi-compat.h`).

The Sv39 tables are real: the kernel runs in M-mode, where satp does not
translate its own accesses, so `mmu-test` proves them through MPRV (M-mode
loads/stores translated as S-mode), including a non-identity alias.

## Milk-V Duo facts (from the duo-buildroot-sdk)

| | |
|---|---|
| timebase | 25 MHz (`cv180x_base_riscv.dtsi`) |
| CLINT | `0x74000000`, **32-bit MMIO only** (`clint,has-no-64bit-mmio`); `mtimecmp` written as split 32-bit halves. **`mtime` (+0xBFF8) is not readable from M-mode** -- measured: load access fault (mcause 5), also with 32-bit loads; the port uses `rdtime` only (`MTIME_READABLE = false`) |
| PLIC | `0x70000000` |
| console | UART4 `0x041C0000`, header pins 4 (TX) / 5 (RX), 115200 8N1 (set by BL2) |
| LED | GPIOC (`0x03022000`) bit 24 (Linux GPIO 440) |
| JTAG | TDI 1, TDO 2, TMS 16, TCK 17, GND 18; IDCODE `0x10000b6f` (T-Head/C-SKY), 1 MHz. Measured: the first OpenOCD attach after a previous session fails (IR capture / `dtmcontrol` 0), the second works -- `hw.sh` retries |

## Build and test

Toolchain: xPack `riscv-none-elf-gcc` 15.2.0-1.1 (found under `~/.local/xPacks`).

```bash
xpm install                      # or have the xPacks installed globally
xpm run build                    # = cmake -S . -B test/build ... && cmake --build
xpm run test                     # emulated set: ctest -LE hwd
xpm run test-hwd                 # every hardware test (test/hw-all.sh)
xpm run hw-reset                 # watchdog reset over JTAG (see below)

test/qemu.sh <app>               # one test on QEMU virt
test/hw.sh <app>                 # one test on the Duo
test/hw-all.sh [app ...]         # all (or the named) hardware tests in a row
```

Under the kernel's harness (`micro-os-plus-iii/tests`, local branch `riscv`):

```bash
xpm run install  --config riscv-milkv-duo-cmake-gcc-debug
xpm run prepare  --config riscv-milkv-duo-cmake-gcc-debug
xpm run build    --config riscv-milkv-duo-cmake-gcc-debug
xpm run test     --config riscv-milkv-duo-cmake-gcc-debug   # 14 qemu cases
xpm run test-<app>-hwd --config riscv-milkv-duo-cmake-gcc-debug
xpm run test-hwd-all  --config riscv-milkv-duo-cmake-gcc-debug   # hw-all.sh
```

### Emulated images

QEMU does not model the CV1800B. Each test's `qemu` image links the port
against QEMU's `virt` machine (`micro-os-plus::riscv-qemu-virt`: virt CLINT,
NS16550 console, virtual LED, own linker script) -- the same sources, run on
different silicon, the way the Cortex-M port runs the pico2's tests.

### Hardware (`hwd`)

Wiring: JTAG TDI pin 1, TDO pin 2, TMS pin 16, TCK pin 17, GND pin 18 (SIPEED
RV CMSIS-DAP, `d6e7:3507`); console UART4 TX pin 4, RX pin 5, 115200 8N1.
Power the Duo from its own USB-C.

1. Boot the Duo from an SD card holding the minimal `fip.bin`
   (`milkv-duo (opensbi)/output/fip.bin`): BL2 brings up clocks, DDR, UART4 and
   the JTAG pinmux, OpenSBI parks the hart.
2. `test/hw.sh <app> [secs]`: OpenOCD alone (no GDB) halts the hart, sets
   M-mode (`reg priv 3`), loads the ELF into DDR at `0x80050000`, enables
   semihosting and resumes at `_start`. Nothing is written to the SD card.
   The verdict has two halves: the `RESULT:` line (UART + semihosting) and the
   **semihosting exit**. With no GDB connected, OpenOCD handles `SYS_EXIT` by
   calling `exit(code)` itself and prints nothing (`semihosting_common.c`), so
   **OpenOCD's own exit status is the test's exit code**. With a GDB
   connected it prints `semihosting: *** application exited with N ***`
   instead; `hw.sh` reads either.
   Codes: 0 PASS, 1 FAIL, 2 no such test / stale build, 3 TIMEOUT,
   4 OPENOCD DIED, 5 NO EXIT. `hw.sh` **refuses an ELF older than any port or
   board source** (a fix was once "tested" with a stale image).
   After a clean exit the hart is left debug-halted on the exit `ebreak`; the
   next session attaches to it directly, no power-cycle needed.
3. `test/hw-all.sh [app ...]`: every hardware test in a row, `GAP` (3-5 s,
   default 4) between tests. `RESET` selects what happens after a test that
   did not end cleanly (TIMEOUT, NO EXIT, OPENOCD DIED):
   - `manual` (default): the board is power-cycled **by hand**;
     `wait-power-cycle.sh` asks for it, waits for BL2's USB device
     `3346:1000` to appear (`DETECT=none`: press ENTER instead), then waits
     `POWERUP_WAIT` (5-10 s, default 8). Measured once: the USB appearance
     was not seen after a power-cycle -- use `DETECT=none` if so.
   - `wdt`: the watchdog reset below, before every test.
   - `0`: no reset; a stuck test ends the run.

   Summary in `<build>/.hw-logs/hw-all-<date>.txt`.

#### The reset (no reset line on the header)

`test/boards/milkv-duo/reset.cfg` (`hw-reset.sh`) uses BL2's own
`__system_reset()`: the RTC domain's watchdog-reset path plus the TOP
watchdog (DesignWare WDT at `0x03010000`, ~166 ms):

| write | meaning |
|---|---|
| `0x050260E0 <- 1` | rtc_core watchdog reset enable |
| `0x050260C8 <- 1` | rtc_core power cycle enable |
| `0x050250AC <- 0` | rtcsys_rstn_src_sel |
| `0x05025004 <- 0xAB18` | `RTC_CTRL0` unlock |
| `0x05025008 <- 0x00400040` | rtc_ctrl watchdog reset enable |
| `0x03010004 <- 0x66`, `0x0301001C <- 0x20` | WDT timeout |
| `0x0301000C <- 0x76` | WDT restart |
| `0x03010000 <- 0x11` | WDT enable, reset on timeout |

Measured: it brought the board back 4 times out of 4 from the parked OpenSBI
state, and **not** after a stuck test (JTAG chain read all ones afterwards).
The SDK's RTC warm reset (`cvi-reboot.c`) reset the SoC but the board did not
come back. A hung board is therefore power-cycled by hand (or pin 30 RUN).

## Tests (test/milkv-duo)

| test | from | what |
|---|---|---|
| mos-test1 | nucleof411 | producers/consumers, mutex, recursive mutex, semaphores, LED |
| sc-test-ko | pico2 | every kernel object, single core |
| mutex-ceiling-test | native | refused protect lock leaves nothing behind |
| smp_test0 | all ports | Phase-1 bring-up: 10 heartbeats (one hart is the whole system) |
| clock-test | all ports | sysclock / hrclock against `rdtime`; CLINT offset |
| exc-test | RP2350 (RISC-V form) | illegal insn, ecall, access fault mid-thread; regs survive |
| mmu-test | RISC-V | Sv39 through MPRV, alias both directions, misaligned |
| semihosting-test | RISC-V | WRITEC/WRITE0, value-returning calls, with the tick running |
| blinky, fp-switch, instrumentation, mutex-stress, rtos-apis, cmsis-os-validator | harness suites | built under the kernel's harness only |

Not carried: the SMP tests (`smp_test1..4`, `smp-*`) -- µOS++ drives one hart
on the Duo and they cannot be built at `OS_NCPU=1` (the second C906 is a
separate, non-coherent core); `sd_test`, `usb_*` -- no SD/USB drivers yet.

### Status (2026-11-05)

Kernel harness, QEMU virt (`xpm run test --config ...`):

| config | result |
|---|---|
| `riscv-milkv-duo-cmake-gcc-release` | **14/14 passed** |
| `riscv-milkv-duo-cmake-gcc-debug` | 13/14 -- all but `cmsis-os-validator` in that run |

`cmsis-os-validator` passed all 60 cases in 3 of the 5 runs made with the
current port. Its thread-to-ISR cases (`TC_MsgFromThreadToISR`,
`TC_MailFromThreadToISR`) are intermittent: when the ISR's sequence counter
gets out of step with the thread's, every later message fails (a hang until
the timeout), or the validator's debug `printf` inside the ISR aborts on
`malloc`. The same cases fail intermittently on the Arm boards (their saved
logs show `!PASSED` in several runs). Not yet root-caused.

QEMU after the `ecall` switch: port tree 8/8, harness release 14/14.

Hardware, Milk-V Duo, port tree (`test/hw.sh`), after the `ecall` switch:

| test | result |
|---|---|
| smp_test0 | **PASS**, exit 0 -- 10 heartbeats, 1004.5 ms apart |
| mos-test1 | **PASS**, exit 0 -- 10 threads, 65 producer/consumer handoffs |
| the others | not yet run on the board |

Before the switch rewrite both stopped at their first blocking call (the MSIP
window described under *The port*).

### What the RISC-V port found in the shared code

The first 64-bit, first non-Arm run of several suites; fixed on the kernel's
local `riscv` branch:

- `src/diag/trace.cpp`: the alias mangling assumed Arm's `va_list`.
- `tests/sources/instrumentation`: a message queue of `sizeof (int)` slots,
  below the kernel's `sizeof (void*)` minimum on 64-bit.
- `tests/sources/mutex-stress`: a 4000-byte main stack, too small for RV64
  (8-byte registers, 528-byte trap frames).
- `tests/sources/rtos-apis`: its C-API block test opened `/dev/mb2`, a device
  that does not exist (`mb-p2`); every Arm platform defines
  `OS_USE_SEMIHOSTING_SYSCALLS`, so the block had never been compiled.
