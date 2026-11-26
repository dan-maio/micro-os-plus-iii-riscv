/*
 * uart.hpp -- QEMU virt console: NS16550A UART0 at 0x10000000 (-serial).
 *
 * The same interface as a board's uart.hpp, so a board's tests compile
 * unchanged for the emulated image.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#if __riscv_xlen == 64
#define PORT_BANNER_LONG "QEMU virt (RV64GC)"
#else
#define PORT_BANNER_LONG "QEMU virt (RV32IMAC, Hazard3 code)"
#endif
#define PORT_BANNER_SHORT "QEMU virt"
#if __riscv_xlen == 64
#define PORT_BANNER_ISA "RV64GC"
#define PORT_BANNER_CPU "rv64"
#else
#define PORT_BANNER_ISA "RV32IMAC"
#define PORT_BANNER_CPU "rv32"
#endif

namespace uart
{
  void
  init (std::uint32_t baud);

  void
  put_char (char c);

  void
  write (const char* s);

  void
  write (const char* s, std::size_t n);

  // The virt console is the UART alone (no semihosting copy).
  inline void
  write_uart_only (const char* s)
  {
    write (s);
  }

  // The console as the common end of a portable test names it
  // (tests/board-tests/portable-test-end.cpp, the AArch boards' uart1):
  // uart1.puts_uart() writes to the UART only. The verdict reaches
  // semihosting separately (semihosting::report_result).
  struct console
  {
    void
    puts_uart (const char* s) const
    {
      write_uart_only (s);
    }
  };

  inline constexpr console uart1{};
} // namespace uart
