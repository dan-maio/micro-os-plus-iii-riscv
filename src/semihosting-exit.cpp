/*
 * semihosting-exit.cpp -- strong _Exit(): end the run through semihosting
 * SYS_EXIT, so std::exit() reports a real exit code.
 *
 *   QEMU      SYS_EXIT ends the emulator with the code; CTest reads it.
 *   hardware  OpenOCD, attached with semihosting enabled, sees the exit.
 *   neither   the trap handler finds no debugger (semihosting.hpp) and the
 *             hart parks in wfi.
 *
 * The AArch32, AArch64 and Cortex-M ports carry the same file.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */

#include <semihosting.hpp>

extern "C" void _Exit (int code) __attribute__ ((noreturn));

extern "C" void
_Exit (int code)
{
#if defined(SEMIHOST)
  semihosting::exit (code);
#else
  (void) code;
  for (;;)
    {
      __asm__ volatile("wfi");
    }
#endif
}
