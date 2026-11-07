/*
 * riscv-board.hpp -- WCH CH32V307V-EVT-R1 (CH32V307VCT6, QingKe V4F).
 *
 * NOT a CLINT machine: the QingKe V4F has no mtime/mtimecmp and no machine
 * timer or software interrupt (mie/mip read 0; mcycle, cycle and time read
 * 0 -- measured with a CSR probe run from SRAM). Its interrupts go through
 * the PFIC (0xE000E000): SysTick is interrupt 12, the software interrupt 14,
 * the peripherals 16 and up. So no CLINT_* constants are given here and
 * clint.hpp does not compile for this board: the kernel build needs the
 * PFIC path of the port first (the preemptive PFIC/SWI scheduler). Until
 * then this board builds only kernel-free tests (BOARD_TEST_NO_KERNEL).
 *
 * The time base is the 64-bit SysTick counter at HCLK (144 MHz).
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace riscv_board
{
  inline constexpr std::uint32_t TIMEBASE_HZ = 144000000u;
} // namespace riscv_board

extern "C"
{
  void riscv_board_initialize_early (void);
  void riscv_board_initialize (void);
  void riscv_board_console_write (const char* s, std::size_t n);
}
