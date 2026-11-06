/*
 * board.cpp -- StarFive VisionFive 2 Lite start-up hooks for the RISC-V port.
 *
 * What runs before µOS++: BootROM -> U-Boot SPL (clocks, DDR training) ->
 * OpenSBI -> U-Boot, stopped at its prompt. OpenOCD then halts the U74
 * harts, loads the test into DDR and starts hart BOOT_HART at _start in
 * M-mode (hw.sh). The other harts stay debug-halted. Clocks, DDR and UART0
 * are up; the board only makes sure nothing OpenSBI/U-Boot left armed for
 * this hart can fire.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */

#include <riscv-board.hpp>
#include <riscv-csr.hpp>
#include <uart.hpp>
#include <board.hpp>

namespace
{
  // PLIC (SiFive): enable bits at 0x2000 + 0x80 * ctx, threshold at
  // 0x200000 + 0x1000 * ctx. 136 sources -> 5 enable words.
  constexpr std::uintptr_t PLIC_ENABLE
      = riscv_board::PLIC_BASE + 0x2000u + 0x80u * riscv_board::PLIC_M_CONTEXT;
  constexpr std::uintptr_t PLIC_THRESHOLD
      = riscv_board::PLIC_BASE + 0x200000u
        + 0x1000u * riscv_board::PLIC_M_CONTEXT;
  constexpr unsigned PLIC_ENABLE_WORDS = (136u + 1u + 31u) / 32u;

  inline volatile std::uint32_t&
  reg (std::uintptr_t addr)
  {
    return *reinterpret_cast<volatile std::uint32_t*> (addr);
  }
} // namespace

// Where hw.sh parks harts 2..4 (the U74s this build does not drive) before
// it resumes the SMP group: a jump to itself, nothing else. Its own section,
// kept by linker.ld, since nothing in the image calls it. Never wfi.
__asm__ (".section .text.park, \"ax\"\n"
         ".globl riscv_board_park\n"
         ".type riscv_board_park, @function\n"
         "riscv_board_park:\n"
         "  j riscv_board_park\n"
         ".size riscv_board_park, . - riscv_board_park\n"
         ".previous\n");

extern "C" void
riscv_board_initialize_early (void)
{
  // Every source off for this hart's M-mode context, threshold at the
  // maximum (7): the port does not use external interrupts yet.
  for (unsigned i = 0; i < PLIC_ENABLE_WORDS; ++i)
    {
      reg (PLIC_ENABLE + 4u * i) = 0;
    }
  reg (PLIC_THRESHOLD) = 7;
  RISCV_CSR_CLEAR (mie, riscv::csr::MIE_MEIE);
}

extern "C" void
riscv_board_initialize (void)
{
  uart::init (board::UART_BAUD);
}
