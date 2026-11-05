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
        │                                   hw-reset.sh + reset.cfg, include/, src/
        └── milkv-duo/                      one directory per test application
```

## The port

| Cortex-M (upstream) | this port |
|---|---|
| PendSV | machine software interrupt (CLINT MSIP) |
| SysTick | machine timer (CLINT `mtimecmp` against `rdtime`) |
| PRIMASK | `mstatus.MIE` (`csrrci`/`csrrsi`, atomic) |
| exception frame + r4-r11 | full frame by `trap.S`: x1..x31, mepc, mstatus, fcsr, f0..f31 |
| IPSR != 0 | `riscv_trap_nesting != 0` |
| NVIC (validator's IRQ 0) | software IRQ lines multiplexed on MSIP (`soft-irq.cpp`) |

`reschedule()` raises MSIP; the trap that follows saves the running thread,
runs `switch_stacks()` and returns on the incoming thread's frame. The tick
is re-armed on a fixed grid of `rdtime` deadlines; the CLINT `mtime - rdtime`
offset is measured at start (`clock-test` reports it), not assumed.

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
| CLINT | `0x74000000`, **32-bit MMIO only** (`clint,has-no-64bit-mmio`; OpenSBI `clint_time_rd32`) |
| PLIC | `0x70000000` |
| console | UART4 `0x041C0000`, header pins 4 (TX) / 5 (RX), 115200 8N1 (set by BL2) |
| LED | GPIOC (`0x03022000`) bit 24 (Linux GPIO 440) |
| JTAG | TDI 1, TDO 2, TMS 16, TCK 17, GND 18 |

## Build and test

Toolchain: xPack `riscv-none-elf-gcc` 15.2.0-1.1 (found under `~/.local/xPacks`).

```bash
xpm install                      # or have the xPacks installed globally
xpm run build                    # = cmake -S . -B test/build ... && cmake --build
xpm run test                     # emulated set: ctest -LE hwd
xpm run test-hwd                 # every hardware test (test/hw-all.sh)
xpm run hw-reset                 # warm-reset the Duo over JTAG

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
2. `test/hw.sh <app>`: OpenOCD halts the hart, sets M-mode (`reg priv 3`),
   loads the ELF into DDR at `0x80050000`, enables semihosting and resumes at
   `_start`. The verdict is the `RESULT:` line (UART + semihosting) and the
   semihosting exit. Nothing is written to the SD card.
3. `test/hw-all.sh [app ...]`: every hardware test in a row. Before each one
   the Duo is **warm-reset over JTAG** and the script waits 3-5 s (`GAP`,
   default 4) for it to boot again, so every test starts on a freshly booted
   SoC. Exit codes per test: PASS, FAIL, MISSING, TIMEOUT, OPENOCD DIED; a
   summary goes to `<build>/.hw-logs/hw-all-<date>.txt`. `RESET=0` replaces
   the reset by a plain pause; `KEEP_GOING=1` continues after a hung test.

#### The reset (no reset line on the header)

`test/boards/milkv-duo/reset.cfg` requests the SoC's own warm reset, the
sequence of the SDK's Linux restart handler
(`linux_5.10/drivers/power/reset/cvi-reboot.c`) and U-Boot's
`cv_system_reset()`:

| step | write | meaning |
|---|---|---|
| 1 | `0x0502603C <- 1` | RTC prdata from the 32 kHz domain |
| 2 | `0x05025004 <- 0xAB18` | unlock `RTC_CTRL0` |
| 3 | `0x050260CC <- 1` (read back) | `RTC_EN_WARM_RST_REQ` |
| 4 | `0x05025008 <- 0xFFFF0810` | `RTC_CTRL0`: warm reset (bit 4) |

The reboot drops JTAG (TMS/TCK are on the UART0 pads until BL2 muxes them),
so that OpenOCD session ends with `shutdown` and the next test attaches anew.
`test/boards/milkv-duo/hw-reset.sh [secs]` does just the reset and the wait.

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

Hardware: all images build; **not yet run on the Duo** (`hw-all.sh`,
`reset.cfg` and the board drivers are untested on silicon).

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
