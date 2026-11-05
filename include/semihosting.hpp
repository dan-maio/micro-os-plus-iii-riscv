/*
 * semihosting.hpp -- RISC-V semihosting (the Arm semihosting protocol over
 * the RISC-V "magic" ebreak sequence).
 *
 * The trap is three uncompressed instructions, on one page:
 *
 *     slli x0, x0, 0x1f      # 0x01f01013  entry marker
 *     ebreak                 # 0x00100073
 *     srai x0, x0, 7         # 0x40705013  exit marker (7 = semihosting)
 *
 * a0 holds the operation, a1 the argument; the result returns in a0. Both
 * OpenOCD (`arm semihosting enable` on a riscv target) and QEMU
 * (-semihosting-config enable=on) recognise it.
 *
 * WITHOUT a debugger the ebreak is an ordinary breakpoint exception. The
 * port's trap handler recognises the sequence, skips it and marks
 * semihosting absent (a0 = -1), so a semihosted build run bare does not
 * fault on its first character -- the same guard the Cortex-M port has in
 * debug-host.h.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

#include <cstdint>

extern "C"
{
  // Set by the trap handler the first time a semihosting call traps instead
  // of reaching a debugger. From then on calls return -1 at once.
  extern volatile bool riscv_semihosting_absent;
}

namespace semihosting
{
  enum op : long
  {
    SYS_WRITEC = 0x03,
    SYS_WRITE0 = 0x04,
    SYS_EXIT = 0x18,
  };

  // ADP_Stopped_ApplicationExit
  inline constexpr std::uint64_t ADP_STOPPED_APPLICATION_EXIT = 0x20026;

  inline long __attribute__ ((noinline))
  call (long operation, const void* arg)
  {
    if (riscv_semihosting_absent)
      {
        return -1;
      }
    register long a0 __asm__ ("a0") = operation;
    register const void* a1 __asm__ ("a1") = arg;
    __asm__ volatile(".option push\n"
                     ".option norvc\n"
                     ".balign 16\n"
                     "slli x0, x0, 0x1f\n"
                     "ebreak\n"
                     "srai x0, x0, 7\n"
                     ".option pop\n"
                     : "+r"(a0)
                     : "r"(a1)
                     : "memory");
    return a0;
  }

  inline bool
  attached (void)
  {
    return !riscv_semihosting_absent;
  }

  inline void
  write0 (const char* s)
  {
    call (SYS_WRITE0, s);
  }

  inline void
  writec (char c)
  {
    call (SYS_WRITEC, &c);
  }

  // RV64 is a 64-bit target: SYS_EXIT takes a two-word parameter block.
  [[noreturn]] inline void
  exit (int code)
  {
    const std::uint64_t block[2]
        = { ADP_STOPPED_APPLICATION_EXIT,
            static_cast<std::uint64_t> (static_cast<std::int64_t> (code)) };
    call (SYS_EXIT, block);
    for (;;)
      {
        __asm__ volatile("wfi");
      }
  }

  [[noreturn]] inline void
  exit_success (void)
  {
    exit (0);
  }

  [[noreturn]] inline void
  exit_failure (void)
  {
    exit (1);
  }
} // namespace semihosting
