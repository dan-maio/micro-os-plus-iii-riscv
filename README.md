# micro-os-plus-iii-riscv

The **RISC-V port of µOS++ III**: RV64 or RV32, with or without an FPU,
machine mode, one hart or SMP (`OS_NCPU` harts). The fourth
architecture project beside `micro-os-plus-iii-cortexm`, `-aarch32` and
`-aarch64`, built and tested the same way.

First board: **Milk-V Duo (64 MB)** -- Sophgo CV1800B, T-Head C906 @ 850 MHz,
64 MiB DDR2 (RV64GC). Second board: **Raspberry Pi Pico 2** (RP2350) on its
two Hazard3 cores (RV32IMAC, no FPU), µOS++ SMP on both -- see *RV32 and the
Pico 2*, *SMP* and *Pico 2 hardware* below. Third board: **WeAct RP2350B**
(16 MB flash, 8 MB QSPI PSRAM) -- the Pico 2's sources with the data in the
PSRAM, the clock table and the 250 MHz overclock; see *WeAct RP2350B*.
Fourth board: **StarFive VisionFive 2 Lite** (JH-7110S, four SiFive U74
RV64GC harts + one S7 monitor hart), µOS++ SMP on the four U74 in M-mode
(mhartid 1..4 = CPUs 0..3), every test loaded into and run from DDR -- see
*StarFive VisionFive 2 Lite*.

## Layout

```text
micro-os-plu-iii-smp-riscv-pr-github/       the workspace
├── micro-os-plus-iii/                      the kernel (sibling clone)
└── micro-os-plus-iii-riscv/                this port
    ├── CMakeLists.txt                      micro-os-plus::riscv, ::riscv-bare,
    │                                       ::riscv-qemu-virt, ::riscv-qemu-virt32
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
        │                                   wait-power-cycle.sh, include/, src/, usb/
        ├── boards/pico2/                   the RP2350 sources (both RP2350 boards)
        ├── boards/pico2-rp2350b-psram/     board.cmake (includes pico2's),
        │                                   linker.ld (16 MB), linker-psram.ld,
        │                                   hw.sh, qemu.sh
        ├── boards/visionfive2-lite/        board.cmake, linker.ld (DDR),
        │                                   jh7110_daplink.cfg, hw.sh,
        │                                   include/ (multicore.hpp), src/
        ├── milkv-duo/, pico2/,             one directory per test application
        ├── pico2-rp2350b-psram/
        └── visionfive2-lite/
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
| missed ticks | dropped (one pending SysTick) | **caught up**: each tick trap counts the whole periods `rdtime` has passed; N owed ticks = N-1 bare `sysclock`/`hrclock` increments + one `os_systick_handler()`, so timeouts and timers are checked once against the advanced clock (no burst of a periodic timer). The comparator is never set closer than one period from now (`riscv_port_tick_guard`). The kernel's simulated RTC still loses halted time |
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

The tick is re-armed on a fixed grid of `rdtime` deadlines, before the
handler. **After** the handler, `riscv_port_tick_guard()` checks the
deadline again: if the handler itself ran past it (a slow timer callback),
the next tick is set one full period from now, so threads always get at least
one period between ticks. Measured on the Duo without it: `rtos-apis`'
1-tick periodic timer, whose callback `printf`s, kept the hart inside the
tick trap for ever (the next tick was pending before `mret`), and `main`
never ran again.

On the board the console is copied to semihosting **from threads only**. A
semihosting call halts the hart for a JTAG round trip (tens of ms), so output
written inside a trap (a timer callback's `printf`) goes to the UART only.
Verdict lines and exits are written from threads.

**MMU attributes (C906).** BL2 sets `mxstatus = 0xc0638000`, which includes
MAEE (bit 21): the C906 then reads PTE bits 63..59 as memory attributes
(SO, C, B, SH, Sec). With them at 0 a RAM page is non-cacheable: accesses
through the MMU bypass the D-cache that untranslated M-mode accesses use
(measured: `mmu-test` failed on the Duo, passed on QEMU). The board supplies
the values the SDK's Linux uses (`arch/riscv/include/asm/pgtable*.h`):
RAM = `C|B|SH`, devices = `SO|SH` (`riscv_board::FLAGS_RAM/_DEV`). The QEMU
board keeps 0: on a standard Sv39 hart those bits are reserved.

Semihosting is the `slli/ebreak/srai` sequence; without a debugger the trap
handler skips it (a0 = -1) instead of faulting. The kernel's "embedded" code
paths are selected with `__ARM_EABI__`, as on AArch64 (see
`include/riscv-eabi-compat.h`).

The Sv39 tables are real: the kernel runs in M-mode, where satp does not
translate its own accesses, so `mmu-test` proves them through MPRV (M-mode
loads/stores translated as S-mode), including a non-identity alias.

## RV32 and the Pico 2

**XLEN- and FPU-generic.** Nothing in the port assumes RV64 or an FPU:

| | RV64GC (C906) | RV32IMAC, no FPU (Hazard3) |
|---|---|---|
| CSR values | `riscv::reg_t` = 64 bit | 32 bit |
| trap frame (`port_ctx.hpp`) | x1..x31, mepc, mstatus, fcsr, f0..f31: **528 bytes** | x1..x31, mepc, mstatus: **144 bytes** |
| `trap.S`, `startup.S` | `REG_S/REG_L` = `sd/ld`; FP block saved | `sw/lw`; no FP block (`__riscv_flen` undefined) |
| `mcause` interrupt bit | bit 63 | bit 31 (`MCAUSE_INTERRUPT` = top bit of XLEN) |
| 64-bit `time`/`cycle` | one CSR read | `rdtimeh`/`rdtime`/`rdtimeh`, repeated on a carry |
| semihosting `SYS_EXIT` | block `{reason, status}` | reason in a1: status 0 -> `ApplicationExit` (exit 0), else `RunTimeErrorUnknown` (exit 1) |
| MMU | Sv39 (`mmu.cpp`) | none; `mmu-test` reports `RESULT: SKIP` |

**RV32 stand-in.** `micro-os-plus::riscv-qemu-virt32` is the `virt` machine
compiled exactly as the pico-sdk compiles for Hazard3
(`-march=rv32ima_zicsr_zifencei_zba_zbb_zbs_zbkb_zca_zcb_zcmp -mabi=ilp32`).
`qemu.sh` picks the emulator from the image's ELF class; an RV32 image runs on
`qemu-system-riscv32 -cpu rv32,f=false,d=false,zfa=false,zba=true,zbb=true,zbs=true,zbkb=true,zcb=true,zcmp=true`
(QEMU's default rv32 model enables Zfa, which needs F). Any board's emulated
tests can be built for it:

```bash
cmake -S . -B test/build-rv32 -G Ninja -D CMAKE_TOOLCHAIN_FILE=cmake/toolchains/riscv-none-elf.cmake \
      -D BOARD=milkv-duo -D UOS_QEMU_STANDIN=micro-os-plus::riscv-qemu-virt32
cmake --build test/build-rv32 && (cd test/build-rv32 && ctest -LE hwd)
```

A `RESULT: SKIP` with exit 0 is a skip, not a failure (`qemu.sh` returns 77,
CTest's `SKIP_RETURN_CODE`).

**Pico 2 bring-up facts (measured 2026-11-05, Raspberry Pi Debug Probe, SWD):**

| | |
|---|---|
| architecture switch | `OTP.ARCHSEL` (`0x40120158`), sampled per core at warm reset; the boot ROM switches both cores to Hazard3 by itself when the flash image's IMAGE_DEF block says RISC-V (datasheet §3.9). Read back after flashing a RISC-V image: `ARCHSEL = ARCHSEL_STATUS = 3` |
| OpenOCD | **the Raspberry Pi build** (`~/.pico-sdk/openocd/0.12.0+dev`, `target/rp2350-riscv.cfg`). xPack OpenOCD 0.12.0-7.1 cannot: its `riscv` target rejects `-dap` (RISC-V debug over the Arm DAP) |
| cores | both examined: XLEN=32, `misa=0x40901105`; core 1 waits in the boot ROM (pc `0x76de`) until launched |
| semihosting | works; `SYS_EXIT` (32-bit form) ends OpenOCD with exit status 0, as on the Duo |
| probe | Debugprobe firmware 2.0.1: OpenOCD warns it is old and uses a slower workaround |
| time | Hazard3 does **not** implement the `time`/`timeh` CSRs (datasheet, RVCSR `MCOUNTEREN.TM`): the clock is the SIO platform timer `MTIME`/`MTIMEH` (`0xd00001b0`), one counter shared by both cores; `MTIMECMP`/`MTIMECMPH` (`0xd00001b8`) are per core at the same address |
| soft IRQ | SIO `RISCV_SOFTIRQ` (`0xd00001a0`): bit h sets core h's MSIP, bit 8+h clears it (not a CLINT word per hart) |

## SMP (OS_NCPU > 1)

The kernel's SMP contract (`OS_USE_SMP_SCHEDULER`, defined by `uos_add_app`
when NCPU > 1) and how the RISC-V port meets it -- the same model as the
RP2350 Cortex-M port, on RISC-V mechanisms:

| | RP2350 Cortex-M port | RISC-V port |
|---|---|---|
| CPU id (`port_cpu_id()`) | `SIO_CPUID` | `mhartid` (`riscv::this_hart()`) |
| kernel lock | recursive spinlock on SIO hardware spinlock 0 | recursive spinlock on an AMO (`amoswap.w.aq` / store-release), owner + depth; the lock word lives where AMOs work (`include/riscv-amo.hpp`, see *WeAct RP2350B*) |
| critical section | PRIMASK + kernel lock | `mstatus.MIE` + kernel lock |
| scheduler lock | per core, holds the kernel lock with IRQs masked | the same, per hart |
| switch | PendSV per core | `ecall` per hart (as single-hart) |
| frame publish | `stack_ptr` published at once, incoming claimed (`nullptr`) | the same: trap.S saves the frame on the thread's stack, the dispatcher runs on the hart's own interrupt stack |
| IPI (`port_smp_ipi()`, `OS_SMP_IPI_SGI`) | SIO FIFO IRQ | the target hart's machine software interrupt (mcause 3) |
| tick | per-core SysTick; core 0 advances the clock | per-hart `mtimecmp`; hart 0 calls `os_systick_handler()`, the others only reschedule |
| secondary start | boot-ROM FIFO handshake | QEMU virt: every hart enters `_start`, harts > 0 wait in `riscv_secondary_park` for their slot `{magic, entry, sp}`; RP2350: the boot ROM FIFO (board) |
| per-core data | `lock_state[]`, core-1 MSP stack | `lock_state[]`, `riscv_trap_nesting[]`, `riscv_port_pending[]`, `riscv_port_yield_deferred[]`, `riscv_irq_stack_top[]` |

"Inside a trap" is read safely on SMP: a trap always runs with MIE clear, so
with MIE set it is thread code, and with MIE clear the thread cannot move to
another hart while it reads its hart's nesting count.

Tests launch the second hart the way the Cortex-M Pico 2 tests do, with the
same names (`include/riscv-smp.hpp`): register `idle1` in
`os_idle_thread_core[1]`, then
`multicore::launch_core1 (port_smp_secondary_start, port_core1_stack_top ())`.

`riscv_sw_log[256]` records the first 256 switches (hart, outgoing and
incoming thread, outgoing state), for a debugger.

A board without a hardware linker script builds only `qemu` images
(`UOS_BOARD_LINKER_HW ""`); `board_test_qemu_ncpu()` lets an SMP board's QEMU
images keep their hart count; `qemu.sh` starts as many harts as the image was
built for (`riscv_secondary_boot`'s size), one for a single-hart image.

## Pico 2 hardware (test/boards/pico2)

| | |
|---|---|
| image | runs from flash (XIP, `linker.ld`); `src/image-def.S` puts the RISC-V IMAGE_DEF block (EXE / RISC-V / RP2350, entry `_start`, sp) first, inside the 4 KiB the boot ROM scans; on an Arm boot the ROM switches both cores to Hazard3. `startup.S` now copies `.data` from `__data_load` (`LOADADDR(.data)`; equal to `__data_start` on RAM images) |
| clocks | `src/clocks.cpp` (the Cortex-M pico2 sequence and API): XOSC 12 MHz -> PLL_SYS -> clk_sys = clk_peri, 150 MHz by default; `PICO2_SYS_CLK_KHZ` = 48000 / 100000 / 120000 / 150000 / 200000 / 250000 (200 and 250 MHz raise the core voltage first, POWMAN VREG 1.20 / 1.25 V); `PICO2_CLOCK_FROM_SCRATCH`: a row of `clocks::SYS_TABLE` (48..200 MHz, + 250 MHz with `PICO2_ENABLE_250MHZ`) from the watchdog scratch, 48 MHz on a cold boot, `clocks::reboot_to_index()` reboots into another row. The QSPI dividers are set before the clock changes and **only raised** (flash SCK <= 50 MHz, PSRAM SCK <= 109 MHz): measured, lowering the flash divider to 1 on the boot clock made the next fetch garbage (mcause 2 inside `clocks::init()`). TICKS from clk_ref: watchdog, TIMER0 and the RISC-V timer at 1 us, whatever clk_sys is. `clocks::adc_clk_init()` (clk_adc from PLL_USB), `src/adc.cpp` |
| time | SIO `MTIME` (`MTIME_CTRL` EN, FULLSPEED off): **1 MHz** (`TIMEBASE_HZ`); `TIME_CSR = false` (Hazard3 has no `time` CSR) |
| comparator / IPI | SIO `MTIMECMP` per core at one address (stride 0); `RISCV_SOFTIRQ` set/clear bits (`RISCV_BOARD_SOFTIRQ_SIO` in `clint.hpp`) |
| console | UART0 PL011, GPIO0 TX / GPIO1 RX (the Debug Probe's UART; `PICO2_UART_TX_PIN` / `_RX_PIN`), 115200, the divider from the running clock (`clocks::current_khz()`); shared by both harts under SIO spinlock 1 with MIE masked; `uart::get_char()` non-blocking |
| LED | GPIO25 (`PICO2_LED_PIN`) |
| libraries | the toolchain has no multilib for the Hazard3 `-march`, and alone picks `rv32ia` -- **no M**: libgcc's software float multiplied through a shift-and-add `__mulsi3`, four calls per float multiply. `board.cmake` links `-march=rv32imc_zicsr_zifencei` (`UOS_BOARD_LINK_FLAGS`, link only; same ilp32 ABI). Measured on the RP2350B, smp-mat-test N=300: parallel 49.99 s -> **14.44 s**, classical 57.96 s -> **23.95 s**, 2-core ratio 1.16x -> **1.66x**. Hazard3 has no FPU: float stays in software |
| core 1 | `src/multicore.cpp`: the boot ROM FIFO handshake `{0, 0, 1, mtvec, sp, entry}` (pico-sdk `multicore_launch_core1_raw`, `h3.unblock` for SEV) into `src/secondary.S` (loads gp and mtvec -- the ROM does not set gp), then the caller's entry. **No PSM reset of core 1**: measured, forcing it off/on under the debugger gives `[rp2350.rv1] Hart unexpectedly reset!` and no handshake; after a chip reset it already waits in the ROM (as the pico-sdk assumes) |
| interrupts | Hazard3 Xh3irq as the NVIC (`src/irq.cpp`, `include/rp2350-irq.h`, generated from the pico-sdk's `intctrl.h`): `NVIC_Enable/DisableIRQ` = `meiea` (per core) + `mie.MEIE`, `NVIC_Set/ClearPendingIRQ` = `meifa` (forced pending; a `meinext` read that returns the IRQ clears it), `NVIC_SetPriority(n, p)` = `meipra` 15 - p; the trap's external interrupt loops on `meinext` and calls `<NAME>_Handler` (the Cortex-M vector names, weak; an unhandled IRQ is disabled). The port's soft-IRQ `NVIC_*` are weak and stay the Duo's |
| nested interrupts | the pico-sdk's dispatcher: save `meicontext` with CLEARTS, then per `meinext` (with UPDATE, which raises the preemption threshold to that IRQ's priority) set `mstatus.MIE`, call the handler, clear MIE; restore `meicontext`. A higher-priority IRQ preempts a running handler. The port reads "inside a trap" with MIE briefly cleared on SMP (`riscv::in_trap()`), and `reschedule()` restores MIE in a handler. `smp-test-nested` proves it: TIMER0 alarm 2 (priority 8) preempts the UART0 handler (12), counted per core |
| console to semihosting | **verdict lines only** (a write containing `RESULT:`); the rest is UART only. Measured: with every line copied, OpenOCD + the Debug Probe (fw 2.0.1) stopped servicing semihosting in long runs (validator: core 0 halted on a valid `SYS_WRITE0` after 43; smp-mat-test lost the debug module after ~114). The trap report also goes to semihosting (mcause, mepc, mtval, ra) |
| `mtval` | Hazard3 hardwires it to 0 (datasheet): code that needs a fault address decodes the instruction (the validator's DWT emulation does) |
| harness | kernel platform `riscv-pico2` (`xpm run ... --config riscv-pico2-cmake-gcc-{debug,release}`); `test/pico2/include/cmsis-plus/os-app-config.h` caps rtos-apis's RTOS arena at 96 KiB on hardware and leaves FatFs out (the free store is 468992 bytes; 512 KiB threw bad_alloc), as the Cortex-M Pico 2 board does |
| `hw.sh` | Raspberry Pi OpenOCD (`~/.pico-sdk/openocd/*`, also looked up on the mounted home), probe pinned by VID:PID **2e8a:000c** (with the Duo's Sipeed probe also plugged in, OpenOCD took that one first) and, when the VisionFive 2's "JTAG probe on Pico" (same VID:PID) is also plugged in, by the Debug Probe's serial number; `init; reset init; program <elf> verify; reset init; arm semihosting enable` (both harts)`; resume`; verdict = RESULT line + OpenOCD's exit status (SYS_EXIT) |

## WeAct RP2350B (test/boards/pico2-rp2350b-psram)

The RP2350B (48 GPIOs) with 16 MB flash and an 8 MB APS6404 QSPI PSRAM. Its
`board.cmake` includes the Pico 2's (so `src/` and `include/` are shared) and
sets its own linker scripts, pins and greeting.

| | |
|---|---|
| probe | its own CMSIS-DAP, **c251:f001**, 1000 kHz -- nothing is shared with the Pico 2's Debug Probe (2e8a:000c) or the Duo's Sipeed (d6e7:3507); `hw.sh` pins it by VID:PID and disables the GDB/telnet/TCL ports, so it runs beside the Pico 2's session |
| flash | `set FLASHSIZE 0x1000000` (the default 4 MB silently truncates); `linker.ld` is the Pico 2's with 16 MB |
| console, LED | UART0 on **GPIO12 TX / GPIO13 RX**, LED GPIO25 (`PICO2_UART_TX_PIN=12`, ...) |
| Arm view | if `rp2350-riscv.cfg` cannot examine the harts, `hw.sh` first programs the image through `target/rp2350.cfg`; the boot ROM then switches both cores to Hazard3. Measured: this also brings the board back after a clock test left the harts unreachable (`Examination failed`), with no power cycle |
| PSRAM | `src/psram.cpp` (`PICO2_PSRAM`, CS = GPIO0 = `PICO2_PSRAM_CS_PIN`): the Cortex-M driver -- QMI direct mode, RSTEN/RST, quad enable, M1 read 0xEB / write 0x38 formats and timing, window 1 writable. `init()`/`read_id()` are in `.psram_ram`, run from SRAM (in direct mode the QMI cannot serve flash XIP), with MIE off. `PICO2_PSRAM_COPY` (as on Cortex-M): start-up only copies `.psram_ram`, the application calls `psram::init()`. Fixed: the Cortex-M original popped the direct-mode RX FIFO with `(void) reg (QMI_DIRECT_RX);`, which in C++ does **not** read a `volatile` (GCC: "conversion to void will not access object"); it is a real load now, as in the pico-sdk |
| start-up hook | `startup.S` calls `riscv_board_initialize_memory()` (weak, empty by default) on the boot stack **before** the `.data` copy and the `.bss` zeroing; with `PICO2_PSRAM` it copies `.psram_ram` to SRAM and runs `psram::init()`; a failed read-back ends the run with `RESULT: FAIL (PSRAM init ...)` over semihosting |
| `linker-psram.ld` | `.data`, `.bss` and the heap (thread stacks too) at 0x11000000 (8 MB); SRAM keeps `.psram_ram`, the AMO targets, the boot and IRQ stacks and `.noinit` |
| **AMOs** | measured: an `amoswap.w` to the PSRAM window faults (**mcause 7**, store/AMO access fault) -- the first kernel lock after start-up. The objects the port changes with AMOs (`smp_klock`, the soft-IRQ words) are placed in `.data.amo` / `.bss.amo` (`include/riscv-amo.hpp`): ordinary linker scripts fold them into `.data`/`.bss`, `linker-psram.ld` keeps them in SRAM inside `.psram_ram` (copied from flash, zeros included). The PSRAM images contain no other AMO/LR/SC (checked with objdump) |
| clocks | as the Pico 2's (`PICO2_SYS_CLK_KHZ`, the table, 250 MHz); at 250 MHz the PSRAM divider becomes 3 (83 MHz) |
| trap report | adds the hart, `mstatus` and 16 words of the interrupted stack (a jump to 0 leaves mepc and ra 0) |
| flash writer | `src/flash_rp2350.cpp` (`include/flash_rp2350.h`): the predecessor's `mu_flash_erase / _program / _xip_flush / _core1_lockout` API (micro-os-plus-iii-pico2, `src/xip-flash`) on the pico-sdk's RP2350 sequence -- ROM functions through the RISC-V lookup entry (16-bit address at `0x7dfa`, `RT_FLAG_FUNC_RISCV`); MIE off; XIP cache cleaned (pending PSRAM writes); QSPI pads, QMI M0 timing and the CS1 (PSRAM) registers saved; `connect_internal_flash`, `flash_exit_xip`, erase/program, `flash_flush_cache`; XIP re-entered through the boot ROM's setup stub copied from boot RAM (as the SDK's `flash_enable_xip_via_boot2`), or `flash_enter_cmd_xip` when the stub is empty; everything restored. It lives in `.data.ramfunc` (SRAM on every linker script) and calls out only through registers (checked in the disassembly) |
| why MIE is off | between `flash_exit_xip` and the re-entry nothing can be fetched from flash, and the trap vector, the dispatcher and every handler are in flash: an interrupt there (the 1 ms tick, during a tens-of-ms erase) would fetch from a flash that is not mapped. The other hart is parked in SRAM (`mu_flash_core1_lockout`) with its MIE off for the same reason. `mtime` keeps counting and the tick catches the missed periods up (*The port*, missed ticks) |
| `linker-psram-exec.ld` | psram-exec: the supervisor in flash with its data in SRAM, `.psram_ram` in SRAM, and two applications linked at 0x11000000 (A) and 0x11400000 (B), loaded from flash |
| `linker-flatfs.ld` | the image in the first 1 MiB, `FLASH_FS` (3 MiB) at 0x10100000, `_fs_base / _fs_size / _flash_fs_begin / _flash_fs_end` -- the predecessor's `pico2-rp2350-flatfs.ld` layout; data in SRAM (a flash write takes the PSRAM away too, so the caller's stack must not be there) |

The nested-clock tests are interactive, as on the Cortex-M board: a cold boot
runs at 48 MHz and shows the potentiometer (GPIO40 = ADC0) and the clock it
selects; `y` on the UART reboots through the watchdog into that clock
(`reboot_to_index()`: this hart's MIE off, the other core held off through
PSM `FRCE_OFF`, the watchdog's debug-pause bits cleared) and the test starts
there. After the reboot OpenOCD has lost both harts (`Hart unexpectedly
reset!`), so the verdict is the `RESULT` line on the UART, not `hw.sh`'s exit.

## Milk-V Duo facts (from the duo-buildroot-sdk)

| | |
|---|---|
| timebase | 25 MHz (`cv180x_base_riscv.dtsi`) |
| CLINT | `0x74000000`, **32-bit MMIO only** (`clint,has-no-64bit-mmio`); `mtimecmp` written as split 32-bit halves. **`mtime` (+0xBFF8) is not readable from M-mode** -- measured: load access fault (mcause 5), also with 32-bit loads; the port uses `rdtime` only (`MTIME_READABLE = false`) |
| PLIC | `0x70000000`; hart 0 M-mode = context 0 (enable +0x2000, threshold +0x200000, claim +0x200004); priorities 1..7. The C906's interrupt IDs: SG2002 TRM table 3.2 (UART4 = **48**, as in the CV1800B dtsi; DW Timer0..7 = **79..86**). `src/irq.cpp` (`duo_irq::`): a nested dispatcher -- claim, threshold = the source's priority, `mie.MTIE`/`MSIE` cleared (Hazard3's CLEARTS by hand: the tick stays below every external IRQ), `mstatus.MIE` set around the handler, complete, threshold back. Not the `NVIC_*` names: on this board those are the port's soft-IRQ lines (the CMSIS validator uses them) |
| DW timers | `0x030A0000`, 8 x 32-bit down counters, `0x14` apart (load, current, control [enable, user mode, int mask], EOI-on-read, status); count clock **measured 25 MHz** against `rdtime` (TRM 12.4.3 reset default); gates and resets on after BL2 (`CLK_EN_0..4 = 0xffffffff`) |
| reset button | header pin **30 RUN** to pin **36 3V3(OUT)**: RUN is inverted on the Duo -- a high level holds the chip in reset (datasheet 2.7); not pin 37 (3V3_EN switches the 3.3 V off) |
| `mxstatus` | `0xc0638000` written by BL2: MAEE on (PTE bits 63..59 are T-Head attributes) |
| console | UART4 `0x041C0000`, header pins 4 (TX) / 5 (RX), 115200 8N1 (set by BL2) |
| LED | GPIOC (`0x03022000`) bit 24 (Linux GPIO 440) |
| JTAG | TDI 1, TDO 2, TMS 16, TCK 17, GND 18; IDCODE `0x10000b6f` (T-Head/C-SKY), 1 MHz. Measured: the first OpenOCD attach after a previous session fails (IR capture / `dtmcontrol` 0), the second works -- `hw.sh` retries |
| USB | DWC2 v4.20a at `0x04340000` (`GSNPSID = 0x4f54420a`, 8 device endpoints, UTMI+ 16-bit HS PHY, 3072-word FIFO), PLIC IRQ **30** (SG2002 TRM table 3.2); TinyUSB's DWC2 driver in slave mode (`test/boards/milkv-duo/usb/`), brought up with BL2's `cv_usb_hw_init()` + `dwc2_reconfig_usbd()` sequence |

## StarFive VisionFive 2 Lite (test/boards/visionfive2-lite)

JH-7110S: a U74-MC complex -- hart 0 is the S7 monitor core (rv64imac, no
MMU, not used), harts 1..4 are SiFive U74 (`rv64imafdc_zba_zbb`, Sv39).
µOS++ runs in M-mode on **all four U74** for the SMP tests (smp-test2,
smp-mat-test: `OS_NCPU = 4`, CPU n = mhartid n + 1, `RISCV_HART_BASE = 1`)
and on hart 1 alone for the others (`OS_NCPU = 1`, harts 2..4 debug-halted);
`test/visionfive2-lite/tests.cmake` chooses per test, as the Pico 2's does. The board is left at the **U-Boot prompt** (SPL has trained
the DDR, U-Boot has set up UART0); OpenOCD loads each test into DDR at
`0x40200000` and starts it there -- nothing is written to flash or SD.
Build: `cmake -S . -B test/build-vf2 -G Ninja -D BOARD=visionfive2-lite -D
CMAKE_TOOLCHAIN_FILE=cmake/toolchains/riscv-none-elf.cmake`; hardware images
only (no QEMU stand-in).

| | |
|---|---|
| sources | Linux `jh7110.dtsi` / `jh7110-common.dtsi`, the SiFive U7 core complex manual (`vic_u7_manual`), the JH7110 TRM (preliminary V2), `JH-7110S Bare-Metal.md` |
| flags | `-march=rv64imafdc_zicsr_zifencei_zba_zbb -mabi=lp64d -mcmodel=medany -mtune=sifive-7-series` (the same `rv64imafdc_zicsr_zaamo_zalrsc/lp64d` multilib as the Duo) |
| memory | `linker.ld`: everything -- code, data, stacks, heap, page tables -- in a 32 MiB DDR window at `0x40200000` (OpenSBI's `FW_JUMP_ADDR`); OpenSBI at `0x40000000` is left alone |
| timebase | 4 MHz (`timebase-frequency`) |
| CLINT | `0x02000000`, standard SiFive (64-bit `mtimecmp`/`mtime`). The U7 manual lists no `time` CSR, so the clock is **`mtime` over MMIO** (`TIME_CSR = false`, `MTIME_READABLE = true`); measured by clock-test: `sleep_for(100)` = 99 999 µs, 1000 ticks = 999 999 µs |
| hart index | the port's CPU index is `mhartid - RISCV_HART_BASE` (`include/port_ctx.hpp`; 0 on every other board, 1 here, a board define) in `riscv::this_hart()`, `trap.S` (nesting count, IRQ stack) and `startup.S` (which hart boots, the secondary park and release slot; harts below the base or beyond the last CPU stay parked). The CLINT is indexed by mhartid (hart 0 = S7): `BOOT_HART = RISCV_HART_BASE` offsets `CLINT_MSIP`/`CLINT_MTIMECMP`, so CPU n uses hart n+1's `msip`/`mtimecmp` |
| SMP | CPUs 1..3 park in `startup.S` (`riscv_secondary_park`) and are released by `multicore::launch_core(n, port_smp_secondary_start, port_secondary_stack_top(n))` (`include/multicore.hpp`), after their idle threads `idle1..idle3` are registered in `os_idle_thread_core[]`. Fixed for an FPU SMP board: `startup.S` now enables the FPU (`mstatus.FS`) on every hart **before** the secondaries park -- the trap entry saves the FP registers, so a secondary with FS = Off would fault on its first trap (QEMU rv32 and the Pico 2 have no FPU). A thread pinned with `cpu_affinity()` after creation can be picked up by another core first and stay there (measured in smp-mat-test: solver_c0 on mhartid 4): the tests' threads pin themselves first and yield until they are on their core |
| PLIC | `0x0C000000`, 136 sources; contexts in `interrupts-extended` order (0 = hart 0 M, then M and S per U74): hart h's M-mode context is **2h - 1** (1, 3, 5, 7); start-up (CPU 0, before any release) disables the sources and sets the threshold to 7 on all four |
| MMU | standard Sv39 PTEs (no T-Head attributes): CLINT, PLIC, UART0..2, sys_syscon/sysgpio, aon_syscon/aongpio, and the first GiB of DDR. mmu-test's alias moved to `0x80000000` (`0x40000000` is mapped DDR here) |
| misaligned | the U74 traps misaligned loads/stores (U7 manual, data memory system): mmu-test's misaligned step is not run |
| console | UART0 `0x10000000` (DW 8250, reg-shift 2), GPIO5 TX / GPIO6 RX, 115200 8N1 as U-Boot left it (the divisor is kept) |
| LED | `gpio-leds` on **AON GPIO 3**, active high; AON IOMUX `0x17020000` (TRM 2.8.8): DOEN byte at +0x0 (index 0 = output enabled), DOUT byte at +0x4 (0 low / 1 high) |
| park loop | `riscv_board_park` (`src/board.cpp`, section `.text.park`, kept by `linker.ld`): a jump to itself, used by `hw.sh` |

### OpenOCD on the VisionFive 2 Lite (jh7110_daplink.cfg, hw.sh)

`jh7110_daplink.cfg` is the user's `~/Desktop/jh7110_daplink.cfg`, adapted;
everything below was measured on the board (2026-11-06).

| | |
|---|---|
| probe | DAPLink **0d28:0204**, firmware 0255, **CMSIS-DAP v1 (HID only)**. The cfg also accepts the Sipeed d6e7:3507 (CMSIS-DAP v2, bulk, ~40 KiB/s on the Duo) first, if it is plugged in |
| speed | HID costs one 1 ms USB frame per debug-module word: **3.3 KiB/s** loads (44 KiB in 13.4 s), ~**0.8 s per semihosting call**. Neither TCK (2, 4, 10 MHz all 13.4 s) nor the access path (system bus: same load, slower verify; abstract memory access: not supported) changes it. A test takes 1..7 min, mostly its console lines |
| scan chain | two TAPs as in the original cfg (E24, then U74-MC), but **both read IDCODE `0x07110cfd`**: the expected `0x20000c2f` of the first is not what the chip reports. OpenOCD complains and uses the chain anyway; debugging works |
| debug module | 5 harts (`found 5 harts`), `misa 0x800000000094112f`, progbuf 16, datacount 2; abstract CSR reads/writes unsupported (OpenOCD falls back to the progbuf) |
| targets | four **independent** targets `jh7110.cpu1..4` (`-coreid 1..4`); no `init`/`halt`/`echo` at the end (hw.sh drives the session) |
| no `target smp` | with the SMP group, OpenOCD's semihosting handler resumed one hart of four (`3 harts should remain halted, and 1 should resume`) and the program's registers were wrong after the first call -- every run. Also: in a group, `resume <addr>` sends **every** hart to `<addr>` (three harts ran the test at once on the UART) |
| `reset halt` | optional (`HW_RESET=1 hw.sh <test>`): measured not to touch DDR or UART0 (LCR stays 0x03), and the chip stays stable after it; the default is a plain halt of the U-Boot-booted board. U-Boot's `starfive_visionfive2_defconfig` enables no watchdog |
| **semihosting state** | a hart left halted on the **previous** session's semihosting `exit` ebreak (`dcsr.cause = 1`) did **not** halt on its semihosting ebreaks in the next session, although `dcsr.ebreakm` read 1: runs strictly alternated PASS / hang (UART showing PASS, OpenOCD seeing nothing). `hw.sh` therefore loads the image, runs hart 1 on `riscv_board_park` for 100 ms and stops it with a halt request (`dcsr.cause = 3`) before `reg pc _start; resume` -- then 4/4 back-to-back PASS |
| console copy | `src/uart.cpp` copies each console write to semihosting with **`SYS_WRITE`** (one bulk read of the buffer), not `SYS_WRITE0` (one probe round trip per byte, which on this probe ended in link errors) |
| session | `init` -> halt harts 2, 3, 4, 1 -> `load_image` -> for each hart the image drives: `reg priv 3`, park, resume, 100 ms, halt, `arm semihosting enable` -> (SMP) clear the release slots, `reg pc <entry>; resume` on harts 2..n (they park) -> `reg pc <entry>; resume` on hart 1. The verdict is the semihosting exit (OpenOCD's exit status) checked against the `RESULT` line |
| SMP images | detected from `riscv_secondary_boot` (NCPU = its size / 24). Its slots are **cleared through OpenOCD** before the harts start: the DDR keeps the previous run's, and a stale magic would release a hart before CPU 0 has set it up. Semihosting is enabled on every target: threads on any core print, and each hart halts on its own ebreak |
| diagnostics | every run keeps a timestamped log in `test/build-vf2/.hw-logs/`; on a timeout `hw.sh` asks the live OpenOCD (Tcl port 6666) for hart 1's `pc mcause mepc mtval ra sp mstatus dcsr` and the port's `riscv_semihosting_absent` before stopping it |
| wiring | 40-pin header: TMS 35, TCK 37, TDI 38, TDO 40, GND 39 (pin 36 = GPIO36, not reset, not used); console UART0 TX pin 8, RX pin 10 |

### Second probe: Raspberry Pi "JTAG probe on Pico" (jh7110_picoprobe*.cfg)

Every VisionFive 2 config and script in the repo root has a copy for this
probe, identical except for the probe lines.

`test/boards/visionfive2-lite/hw.sh` (and so every VF2 test under CTest)
takes the probe that is plugged in: `jh7110_daplink.cfg` when the DAPLink
(0d28:0204) is there, else `jh7110_picoprobe.cfg` when a 2e8a:000c with the
USB product name "JTAG probe on Pico" is there, pinned by its serial number
(read from sysfs). `VF2_PROBE=daplink|picoprobe` forces one. The probe is
shown in the run's header line. The Pico 2's `hw.sh` in turn pins its Debug
Probe by serial number, skipping the JTAG probe on Pico, so both can stay
plugged in.

| DAPLink | JTAG probe on Pico |
|---|---|
| `jh7110_daplink.cfg` | `jh7110_picoprobe.cfg` |
| `jh7110_daplink_debug.cfg` | `jh7110_picoprobe_debug.cfg` |
| `jh7110_daplink_semihost.cfg` | `jh7110_picoprobe_semihost.cfg` |
| `jh7110_daplink_srst.cfg` | `jh7110_picoprobe_srst.cfg` |
| `jh7110_wdt_reset.cfg` | `jh7110_wdt_reset_picoprobe.cfg` |
| `jh7110_wdt_disable.cfg` | `jh7110_wdt_disable_picoprobe.cfg` |
| `jh7110_jtag_key.cfg` | `jh7110_jtag_key_picoprobe.cfg` |
| `vf2-reset-to-uboot.sh` | `vf2-reset-to-uboot-picoprobe.sh` |
| `vf2-jtag-stop-uboot.sh` | `vf2-jtag-stop-uboot-picoprobe.sh` |

| | |
|---|---|
| probe | a Raspberry Pi Pico with the "JTAG probe on Pico" firmware: **2e8a:000c**, `CMSIS-DAP v2 Interface` (vendor class, USB bulk) plus a CDC-ACM UART; OpenOCD reports FW 2.0.0, SWD and JTAG supported. The cfg selects `vid_pid 0x2e8a 0x000c` and `backend usb_bulk` |
| same VID:PID | the Pico 2's Raspberry Pi Debug Probe is also 2e8a:000c. With both plugged in, set `PICOPROBE_SERIAL` (this probe: `E4623492931D2230`); the cfg then adds `adapter serial` |
| speed | the cfgs set TCK **10 MHz** (`adapter speed 10000`). Measured 2026-11-07, `hw.sh smp-test2` (82 600-byte image, 4 cores, 32 console lines over semihosting), both PASS: **10 MHz: load 2.2 s (37.4 KiB/s), whole test 16 s**; 2 MHz: load 2.6 s (30.7 KiB/s), 17 s. The DAPLink: 24.6 s (3.3 KiB/s) at any TCK |
| SRST | not checked whether this firmware drives a reset line |
| wiring | the same header pins (TMS 35, TCK 37, TDI 38, TDO 40, GND 39). A session that read both IDCODEs but then only zeros past the chain end (`auto0..18.tap idcode=0x0`, `dtmcontrol is 0`) had **TDI** not reaching the board: TCK, TMS and TDO were working |

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

QEMU does not model the CV1800B (nor the RP2350). Each test's `qemu` image links the port
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
| smp-test1 | pico2 (Cortex-M) | single-core RTOS bring-up: two workers, mutex, LED (the same source as the Pico 2's) |
| smp-mat-test | pico2 (Cortex-M) | the block solver at one hart, N = 300, B = 50 (the RP2350B's size), the barrier's SIO spinlock as an AMO, TIMER0 as `rdtime`: block 87.7 ms, classical 113.6 ms on the C906's FPU (RP2350B Hazard3, software float, two cores: 14.4 s / 24.0 s) |
| smp-test-nested | RP2350B (Cortex-M), hw only | nested PLIC interrupts: DW Timer0 (priority 3, one-shot 500 us into the UART handler) must preempt UART4's handler (priority 1, its TX-empty interrupt started by a thread, busy 2 ms); DW Timer1 periodic (2). PASS needs `t2_in_uart > 0`; prints the DW timer, `rdtime` and `sysclock` elapsed times |
| clock-catchup | RISC-V | `sysclock` follows `rdtime` within 2 % across ten 100 ms masked periods |
| smp-test-usb-cdc-acm | pico2 (Cortex-M), hw only | USB CDC-ACM (`cafe:4013`) on the CV1800B's DWC2 controller (TinyUSB DWC2, slave mode): echoes each line ("you typed: ..."), Escape ends with PASS (`usb-host.py cdc`) |
| smp-test-usb-hid | pico2 (Cortex-M), hw only | generic in/out USB HID (`cafe:4012`) on DWC2: receives 64-byte OUTPUT reports, prints characters on UART4, Escape ends with PASS (`usb-host.py hid`) |
| blinky, fp-switch, instrumentation, mutex-stress, rtos-apis, cmsis-os-validator | harness suites | built under the kernel's harness only |

Not carried: the two-core SMP tests (`smp-test2..5`, `smp-test-ko`, the
nested-clock tests) -- µOS++ drives one hart on the Duo (the second C906 is a
separate, non-coherent core); `sd_test`.

### Status (2026-11-06)

StarFive VisionFive 2 Lite (DAPLink, full console over semihosting, every
test in DDR): **11/11 PASS**.
- SMP on the four U74 (`OS_NCPU = 4`):
  - **smp-test2**: one worker per core, every beat read from mhartid on its
    own hart (8/8 each), global 32;
  - **smp-mat-test** (N = 240, B = 30, 8 blocks): solvers c0..c3 on mhartid
    1..4, parallel 15.9 ms against classical 60.3 ms on core 0 (**3.80x**),
    max difference 0; thread-pool tasks spread over CPUs 0..3. (N = 200,
    B = 50: 12.6 ms against 34.8 ms, 2.76x.)
- One hart (`OS_NCPU = 1`): smp_test0 (4 back-to-back runs), semihosting-test,
  clock-test, clock-catchup (sysclock 1500 ms vs mtime 1499 ms), mmu-test,
  exc-test (50 illegal + 50 ecall traps, registers intact), mos-test1,
  sc-test-ko, smp-test1.
- A run takes 1..7 min, mostly the console over semihosting (~0.8 s per line
  with this probe); the image load is 13..17 s (only the loadable part).
- The port changes for it (`RISCV_HART_BASE`, the FPU on every hart before
  the park, `port_secondary_stack_top()`) leave the other boards as they
  were: Duo QEMU 11/11, Pico 2 QEMU 4/4 (2-hart SMP included).

RV32 (phase 1 of the Pico 2 port): the Duo's tests built for the RV32 stand-in
(`test/build-rv32`): **7 passed, mmu-test skipped** (no MMU on RV32). RV64
unchanged: port 8/8, harness 14/14.

SMP (phase 2): board `pico2` (emulated only so far), `smp-test2` ported from
the Cortex-M Pico 2 test, on `qemu-system-riscv32 -smp 2`: **PASS 8/8**
(both workers, 40 handoffs, worker 1 on hart 1). Single-hart builds
unchanged (RV64 8/8, RV32 7 + skip, harness 14/14).

Pico 2 hardware (phase 3, Raspberry Pi Debug Probe): **`smp-test2` PASS,
twice in a row** -- worker 0 on core 0 and worker 1 on core 1, 20 rounds
each, 40 handoffs, exit 0. A worker's lines are ~650 ms apart in RTOS time on
the board against 500 ms on QEMU (cause not measured yet).

Phase 4 (porting the Cortex-M Pico 2 tests to the Hazard3 board), on the
Pico 2 (Raspberry Pi Debug Probe), one run after the semihosting change:

| test | cores | Pico 2 | QEMU |
|---|---|---|---|
| sc-test-ko | 1 | **PASS** | PASS |
| smp-test1 | 1 | **PASS** | PASS |
| smp-test2 | 2 | **PASS** | PASS |
| smp-test3 | 2 | **PASS** | hw only |
| smp-test4 | 2 | **PASS** | hw only |
| smp-test5 | 2 | **PASS** -- UART0 IRQ 33 on core 1 through Xh3irq, 48848 bytes, 0 dropped | hw only |
| smp-test-ko | 2 | **PASS** | hw only |
| smp-mat-test | 2 | **PASS** -- 120x120, block == classical, 1.373x on 2 cores | hw only |
| exc-test (the RISC-V one) | 1 | **PASS** | PASS |
| fp-switch (harness) | 1 | **PASS** | PASS |
| mutex-stress (harness) | 1 | **PASS** | PASS |
| rtos-apis (harness) | 1 | **PASS** (96 KiB arena, no FatFs) | PASS |
| cmsis-os-validator (harness) | 1 | **PASS** | PASS |
| smp-test0 (bare, no kernel) | 2 | **PASS** -- 5 KiB image, core 1 launched and blinking | hw only |
| smp-mat-test-ram | 2 | **PASS** from SRAM | hw only |
| mutex-stress-ram (harness) | 1 | **PASS** from SRAM | hw only |
| rtos-apis-ram (harness) | 1 | **PASS** from SRAM | hw only |
| cmsis-os-validator-ram (harness) | 1 | **PASS** from SRAM | hw only |

| smp-test-usb-cdc-acm | 2 | **PASS** -- enumerates as `cafe:4013`, echoes the host's line, Escape ends it | hw only |
| smp-test-usb-hid | 2 | **PASS** -- enumerates as `cafe:4012`, HID OUTPUT reports, Escape ends it | hw only |

**All 20 Cortex-M Pico 2 tests pass on the RISC-V Pico 2.** Since then:
`smp-test-nested` (nested Xh3irq interrupts, ported from the RP2350B's
Cortex-M test) **PASS** on the Pico 2 -- `wrk0=20 wrk1=20 uart=11 t2=32
t3=16 t2_in_uart=11` (alarm 2 preempted all 11 UART handler runs) -- and
smp-mat-test **PASS** with the `rv32imc` libraries (6 s for the whole run,
26 s before).

WeAct RP2350B (2026-11-06, probe c251:f001):

| test | cores | RP2350B | notes |
|---|---|---|---|
| sc-test-ko | 1 | **PASS** | |
| exc-test | 1 | **PASS** | |
| smp-test0 (bare) | 2 | **PASS** | |
| smp-test1 | 1 | **PASS** | |
| smp-test2 | 2 | **PASS** | |
| smp-test3 | 2 | **PASS** | |
| smp-test4 | 2 | **PASS** | |
| smp-test5 | 2 | **PASS** | UART0 on GPIO12/13 |
| smp-test-ko | 2 | **PASS** | |
| smp-test-nested | 2 | **PASS** | `t2_in_uart=11` |
| smp-mat-test | 2 | **PASS** | N=300, B=50, matrices in PSRAM, 150 MHz: parallel 14.44 s, classical 23.95 s, 1.66x |
| smp-mat-test_200 | 2 | **PASS** | the same at a fixed 200 MHz (PSRAM 100 MHz); not a Cortex-M test |
| smp-mat-test_250 | 2 | **PASS** | the same at a fixed 250 MHz, 1.25 V (PSRAM 83 MHz); not a Cortex-M test |
| smp-test-nested-clock_200 | 2 | **PASS** (UART) | N=500; pot -> 200 MHz, `y`, watchdog reboot, solver + nested IRQs |
| smp-test-nested-clock_250 | 2 | **PASS** (UART) | N=800; pot -> **250 MHz** overclock, `y`, reboot, solver + nested IRQs |
| smp-test-nested-clock | 2 | not run yet | N=1000 (4 MB in place) |
| psram-exec (bare) | 1 | **PASS** (UART, interactive) | the Cortex-M pico2-pizero board's (the predecessor's `pizero-psram-exec*`): a supervisor copies two self-contained applications into the PSRAM (staged through SRAM, retried until verified, XIP cache cleaned + invalidated) and switches between them on ESC; `x` ends it. RISC-V: Arm's vector table + `VTOR` become a header `{magic, entry, trap}`, `mtvec` = the application's own trap handler while it runs, the entry called through a register; each application takes an `ecall` into its PSRAM trap handler. The disassembly shows no call or address leaving the PSRAM |
| smp-test-xip | 2 | **PASS** | from the predecessor (not in the Cortex-M repo): erase a FLASH_FS sector (reads 0xFF), program a page (reads back through XIP), then core 1 runs |
| smp-test-xip-core1park | 2 | **PASS** | 4/4 rounds of erase + program while core 1 runs from flash, parked in SRAM for each write; its counter moved every round |
| smp-test-xip-fs | 2 | **PASS** | the predecessor's append-log flat-fs (`fs-archive.cpp`, unchanged) on the writer: format, three files added, listed, CRC-32 and bytes checked through XIP, found by name. The predecessor's XIP-classifier step (it parses the stored files as ARM `.so` modules) moves to the loader stage; the three files are kept as payloads |
| fp-switch (harness) | 1 | not run yet | built (the kernel suite through the port tree); the kernel-harness platform is not added yet |

The first eleven passed twice: once with the `rv32ia` libraries and again with
`rv32imc`, both before the clock driver was extended. **Not rerun since that
change** (the clock table, the raise-only QSPI dividers, the UART divider from
the running clock): those eleven and the Pico 2's tests. The flash
group is from the predecessor project (`micro-os-plus-iii-pico2`); its loader
tests (`smp-test-loader-*`, `smp-test-xip-loader-*`) are not ported yet;
the pico2-pizero board's `psram-mat-test-250` (the arrays alone in the PSRAM,
a workaround for that board's PSRAM) is left out -- smp-mat-test and `_250`
cover it here. After the RX FIFO fix in `psram.cpp`, smp-mat-test passed
again; the other PSRAM-data tests were not rerun. One
`smp-test-nested-clock_200` run trapped on the second boot after the reboot
(mcause 2, mepc 0, ra 0, on the boot stack); the next run passed. Not
root-caused; the extended trap report is for the next occurrence.

**USB** (`test/boards/pico2/usb/`): TinyUSB, its RP2040/RP2350 device driver
and the pico-sdk shim, as the Cortex-M board carries them, with the Arm parts
made RISC-V (barriers -> `fence`, PRIMASK critical sections -> `mstatus.MIE`,
`sev` -> `h3.unblock`); the USB IRQ goes through Xh3irq; `clocks::usb_init()`
(PLL_USB -> 48 MHz). Linked only into the two USB tests. The Pico 2 has no
WS2812 (the Cortex-M CDC test was brought up on an RP2350B-MINI-A), so
`ws2812.hpp`'s stubs stand in. The Pico 2's own micro-USB must be connected to
the PC.

The PC's half: `test/boards/pico2/usb-host.py cdc|hid` waits for the device,
then sends a line and Escape -- on the CDC tty (Escape **followed by Return**:
the test reads lines), or as HID OUTPUT reports (`{length, chars}`, 64 bytes,
pyusb; detaching `usbhid` usually needs `sudo` or a udev rule). `hw.sh` starts
it for the two USB tests; `HOST_HELPER=0` leaves it to you.

**Bare runtime** (`micro-os-plus::riscv-bare`, for `BOARD_TEST_NO_KERNEL`):
the board, `startup.S`, `trap.S` and `src/bare/runtime.cpp` -- start-up hooks
that only bring the board up, the static constructors, `_exit` -> semihosting,
and a trap dispatcher with semihosting, the exception hook, the board's
external interrupts and the fault report (no tick, no switch); the test
support headers without the board contract.

**`-ram` tests**: `<name>-ram` is `<name>` linked by `linker-ram.ld`
(everything in SRAM, no IMAGE_DEF, no `.data` copy). `hw.sh` picks the session
from the image's entry: in SRAM -> `reset init`, OpenOCD's work area moved
into the image's NOLOAD heap (0x20078000), `load_image`, `verify_image`,
`reg pc <entry>` on core 0, semihosting on both, `resume`; core 1 stays in
the boot ROM until launched. Nothing is written to flash (smp-mat-test-ram:
92 KiB loaded in 0.66 s).

Porting changes: `bsp/` headers -> the board's; Arm idle `dsb`/`wfi` -> yield
only; `dmb`/`__DMB` -> `fence rw, rw`; smp-mat-test's configuration-error
`wfi` parks end the run with FAIL, and it prints every tenth element of the
solution (all 120 are still compared) -- printing all of them cost one
semihosting round trip per call and once ended with the debug module lost
after line 114 (`halted due to undefined`, `dmcontrol=0x0`; not explained
yet). `hw.sh`'s stale-image check was narrowed (both boards) to the port,
the board and the test's own sources: another test's edit made every image
look stale. Its limit, measured: a NEW header that shadows an old one (the
board's os-app-config.h) changes no recorded dependency, so ninja does not
rebuild and the check passes on a stale image -- clean the target
(`ninja -t clean <app>-hwd`) after adding such a header.

Found on the way, in the runner: `qemu.sh` ran under `pipefail` and tested the
image with `nm ... | grep -q`. `grep -q` exits at the first match, `nm` then
dies of SIGPIPE, and the test reads false -- intermittently. SMP images were
started with `-smp 1` in 3 runs of 5 and hart 1 never existed. The pipes now
read to the end.

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

Hardware, Milk-V Duo, full run (`hw-all.sh`, `RESET=wdt`: BL2 watchdog reset
and 4 s of boot before every test), every test ending in its semihosting exit
with code 0:

| test | port tree | harness |
|---|---|---|
| blinky | -- | **PASS** |
| clock-test | **PASS** | **PASS** |
| cmsis-os-validator | -- | **PASS** |
| exc-test | **PASS** | **PASS** |
| fp-switch | -- | **PASS** |
| instrumentation | -- | **PASS** |
| mmu-test | **PASS** | **PASS** |
| mos-test1 | **PASS** | **PASS** |
| mutex-ceiling-test | **PASS** | **PASS** |
| mutex-stress | -- | **PASS** |
| rtos-apis | -- | **PASS** |
| sc-test-ko | **PASS** | **PASS** |
| semihosting-test | **PASS** | **PASS** |
| smp_test0 | **PASS** | **PASS** |
| total | **8/8** (228 s) | **14/14** (606 s) |

What the first full run found, fixed since: `mmu-test` (C906 MAEE page
attributes), `rtos-apis` (tick-handler overrun; semihosting from a trap),
`semihosting-test` (OpenOCD's SYS_ERRNO starts at -1). See *The port*.

Seen during the run, not a failure: OpenOCD logged `dmstatus=0x0` ("Debug
Module version 0") once in `mutex-stress` and once in `rtos-apis`, the two
tests with the most semihosting traffic; it recovered and both passed.

Before the switch rewrite every blocking test stopped at its first blocking
call (the MSIP window described under *The port*).

Hardware, Milk-V Duo, 2026-11-06, after the RV32/SMP, nested-interrupt, AMO,
start-up-hook changes and the tick catch-up, every test ending in its
semihosting exit 0 (`RESET=0`, back to back): **port tree 12/12** (368 s; the
8 above + clock-catchup, smp-test1, smp-mat-test, smp-test-nested) and
**harness 18/18** (757 s; the 14 + the four, rtos-apis included). Earlier the
same day, before the new tests: port 8/8, harness 14/14.

USB on the Milk-V Duo (`test/boards/milkv-duo/usb/`, 2026-11-06):
**`smp-test-usb-cdc-acm` PASS** (`cafe:4013`, `/dev/ttyACM1`, full banner +
line echo, Escape, exit 0) and **`smp-test-usb-hid` PASS** (`cafe:4012`,
64-byte HID OUTPUT reports, Escape, exit 0) with `test/boards/pico2/usb-host.py`
-- **port tree 14/14** on the Duo. Found and fixed during the DWC2 bring-up:

- **CV1800B DWC2 v4.20a slave mode (`dcd_dwc2.c`)**: `GUSBCFG`
  (`ForceDevMode | (1 << 19) | (5 << TRDT) | PHYIF16`) must be written
  immediately after `dwc2_core_init()` *before* sizing/flushing FIFOs or
  programming device registers (as BL2's `dwc2_reconfig_usbd()` does);
  `dfifo_device_init()` statically programs `GRXFSIZ = 0x400`,
  `DIEPTXF0 = 0x00400400` and `DIEPTXF[0..6]` above `0x440` before flushing,
  and `dfifo_alloc()` is a no-op so `SET_CONFIGURATION` does not rewrite
  `DIEPTXFn` live without a FIFO flush; `DOEPTSIZ0` uses `STUPCNT = 1` and
  `handle_bus_reset()` does not re-flush FIFOs; `dcd_int_handler()` pops one
  packet per `GINTSTS_RXFLVL` interrupt (polling `GRXSTSP` in a tight `do/while`
  underflowed into 32-entry phantom `6/ep0/8` bursts); `dcd_init()` holds
  `DCTL_SDIS` for 300 ms so the upstream hub tears down any prior enumeration.
- **PLIC IRQ 30 starvation (`osal_none.h`)**: `osal_queue_receive()` disabled
  and re-enabled the USB IRQ on every empty-queue poll; preempted by the 1000 Hz
  tick while `usb_device_task` had `plic_en = 0`, PLIC source 30 stayed masked
  throughout UART/semihosting prints. `osal_queue_receive()` now returns `false`
  immediately when `tu_fifo_empty(&qhdl->ff)` is true, and `usb_device_task`
  guards `tud_task()` with `tud_task_event_ready()`.
- **Post-reset SETUP drop (`usbd.c`)**: `usbd_reset()` zeroed
  `_usbd_queued_setup` when `DCD_EVENT_BUS_RESET` was dequeued from `_usbd_q`,
  which wiped the count of post-reset `DCD_EVENT_SETUP_RECEIVED` events already
  queued behind `DCD_EVENT_BUS_RESET` in the FIFO and caused `tud_task()` to
  discard `GET_DESCRIPTOR`.
- **Host helper timing (`milkv-duo/hw.sh`)**: JTAG `halt` stops the C906 core
  without resetting the DWC2 peripheral, so a previous run's `/dev/ttyACM*`
  stays alive during OpenOCD's 2.2 s `load_image`; `hw.sh` starts `usb-host.py`
  only once `[usb] tusb_init done` appears in the log.

Found on the way: the tick dropped missed periods -- smp-test-nested measured
`sysclock` 19 s against 45 s of `rdtime` and DW timer (semihosting halts the
hart for every console line); fixed by the catch-up above, then
`elapsed: DW timer 34066 ms, rdtime 34066 ms, sysclock 34067 ms`. And twice
the same C++ trap: `(void) reg (addr);` on a `volatile` reference performs
**no load** (GCC: "conversion to void will not access object") -- the DW
timer's EOI was never read, its interrupt stayed asserted and the hart lived
in the dispatcher (the PSRAM driver's RX-FIFO pop had the same fault); the
reads go through a helper now, and no build gives the warning.

The Duo's JTAG watchdog reset is not always enough: after a hang, or a
failed reset, the JTAG chain reads all ones until a hand power cycle (pin 30
RUN to 3V3 would do it, see the facts table).

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
