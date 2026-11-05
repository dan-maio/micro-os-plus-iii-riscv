/*
 * riscv-board.hpp -- the facts the RISC-V port asks of a board.
 *
 * Milk-V Duo (64 MB): Sophgo CV1800B, main hart = T-Head C906 @ 850 MHz,
 * 64 MiB DDR2 at 0x80000000.
 *
 * Every number below is taken from the Duo SDK, not from memory:
 *   duo-buildroot-sdk build/boards/default/dts/cv180x_riscv/cv180x_base_riscv.dtsi
 *     timebase-frequency = <25000000>
 *     clint@74000000 { compatible = "riscv,clint0"; clint,has-no-64bit-mmio; }
 *     plic0: interrupt-controller@70000000 { reg = <0x70000000 0x4000000>; }
 *   opensbi lib/utils/timer/fdt_timer_clint.c
 *     { .addr = 0x74000000, .has_64bit_mmio = 0 }  -> mtime/mtimecmp as two
 *     32-bit halves; mtime at +0xBFF8 (lib/utils/sys/clint.c)
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include <mmu.hpp>

namespace riscv_board
{
  inline constexpr std::uintptr_t CLINT_BASE = 0x74000000u;
  inline constexpr std::uintptr_t CLINT_MSIP = CLINT_BASE + 0x0000u;
  inline constexpr std::uintptr_t CLINT_MTIMECMP = CLINT_BASE + 0x4000u;
  inline constexpr std::uintptr_t CLINT_MTIME = CLINT_BASE + 0xBFF8u;
  // clint,has-no-64bit-mmio
  inline constexpr bool MTIMECMP_SPLIT32 = true;

  inline constexpr std::uint32_t TIMEBASE_HZ = 25000000u;

  inline constexpr std::uintptr_t PLIC_BASE = 0x70000000u;

  // Sv39 identity map (mmu.hpp). The MMIO windows are those of the cv1800
  // Eclipse project's mmu.c; DRAM is the Duo's 64 MiB.
  inline constexpr mmu::region MMU_REGIONS[] = {
    { 0x03000000u, 0x00200000u, mmu::FLAGS_DEV }, // TOP: pinmux, clk, rst, GPIO
    { 0x04000000u, 0x01000000u, mmu::FLAGS_DEV }, // HSPERI: UART, SD, USB
    { 0x05000000u, 0x00400000u, mmu::FLAGS_DEV }, // RTC domain
    { 0x0E000000u, 0x00200000u, mmu::FLAGS_DEV }, // AXI SRAM
    { 0x70000000u, 0x00400000u, mmu::FLAGS_DEV }, // PLIC
    { 0x74000000u, 0x00200000u, mmu::FLAGS_DEV }, // CLINT
    { 0x80000000u, 0x04000000u, mmu::FLAGS_RAM }, // DDR2, 64 MiB
  };
} // namespace riscv_board

extern "C"
{
  // Called by the port's start-up (src/startup.cpp).
  void riscv_board_initialize_early (void);
  void riscv_board_initialize (void);
  // The raw console: trace, printf and the fault report all end here.
  void riscv_board_console_write (const char* s, std::size_t n);
}
