/*
 * uart.cpp -- VisionFive 2 Lite console on UART0 (DesignWare APB 8250, 32-bit
 * registers, reg-shift 2).
 *
 * The divisor is NOT reprogrammed: the SPL / U-Boot set UART0 to 115200 8N1
 * (stdout-path = "serial0:115200n8") and that is what the terminal works
 * with. The driver only makes sure the line format and FIFOs are as expected.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */

#include <cstddef>
#include <cstdint>

#include <uart.hpp>

#if defined(HW_BUILD) && defined(SEMIHOST)
#include <semihosting.hpp>
#include <riscv-hart.hpp>
#endif

namespace
{
  constexpr std::uintptr_t UART0_BASE = 0x10000000u;

  constexpr unsigned THR = 0x00; // transmit holding (DLAB=0)
  constexpr unsigned FCR = 0x08; // FIFO control
  constexpr unsigned LCR = 0x0C; // line control
  constexpr unsigned LSR = 0x14; // line status

  constexpr std::uint32_t LSR_THRE = 1u << 5;

  inline volatile std::uint32_t&
  reg (unsigned off)
  {
    return *reinterpret_cast<volatile std::uint32_t*> (UART0_BASE + off);
  }

  void
  raw_put (char c)
  {
    while ((reg (LSR) & LSR_THRE) == 0)
      {
      }
    reg (THR) = static_cast<std::uint8_t> (c);
  }

  void
  raw_write (const char* s, std::size_t n)
  {
    for (std::size_t i = 0; i < n; ++i)
      {
        if (s[i] == '\n')
          {
            raw_put ('\r');
          }
        raw_put (s[i]);
      }
  }

#if defined(HW_BUILD) && defined(SEMIHOST)
  // Copy to semihosting with SYS_WRITE (buffer + length): OpenOCD fetches
  // the whole buffer in one memory read. SYS_WRITE0 makes it read the
  // string one byte per probe round trip, and on this board's probe
  // (DAPLink CMSIS-DAP fw 0255, HID, USB full speed) that traffic killed the
  // JTAG link mid-run twice (2026-11-06: a DMI timeout after 9 bytes, then
  // CMD_DAP_JTAG_SEQ failed after ~200 bytes), while the same image without
  // semihosting ran clean and the probe stayed enumerated.
  // Not from a trap: each semihosting call halts the hart for a JTAG round
  // trip, far longer than a tick. Output written by a handler (a timer
  // callback's printf) goes to the UART only.
  void
  semi_copy (const char* s, std::size_t n)
  {
    if (n == 0 || !semihosting::attached () || riscv::in_trap ())
      {
        return;
      }
    semihosting::write_buf (s, n);
  }
#endif
} // namespace

namespace uart
{
  void
  init (std::uint32_t baud)
  {
    (void) baud; // U-Boot's divisor is kept, see above
    reg (LCR) = 0x03; // 8N1, DLAB=0
    reg (FCR) = 0x07; // FIFOs on, both cleared
  }

  void
  put_char (char c)
  {
    raw_put (c);
  }

  void
  write (const char* s)
  {
    std::size_t n = 0;
    while (s[n] != '\0')
      {
        ++n;
      }
    write (s, n);
  }

  void
  write (const char* s, std::size_t n)
  {
    raw_write (s, n);
#if defined(HW_BUILD) && defined(SEMIHOST)
    semi_copy (s, n);
#endif
  }
} // namespace uart

extern "C" void
riscv_board_console_write (const char* s, std::size_t n)
{
  uart::write (s, n);
}
