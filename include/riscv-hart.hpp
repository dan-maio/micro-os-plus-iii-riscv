/*
 * riscv-hart.hpp -- which hart is this, and is it inside a trap.
 *
 * A hart is identified by mhartid, 0..OS_NCPU-1 (the board guarantees it: the
 * QEMU virt machine and the RP2350's Hazard3 cores number them that way). On
 * a single-hart build the answer is the constant 0 and no CSR is read.
 *
 * "Inside a trap" is the per-hart nesting count kept by trap.S. A trap always
 * runs with mstatus.MIE clear (the hardware clears it on entry and nothing in
 * the port sets it inside a handler), so:
 *   - with MIE set this is thread code: not in a trap, nothing to read;
 *   - with MIE clear the code cannot be moved to another hart while it reads
 *     the count, so the hart id and the count it indexes agree.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

#include <cstdint>

#include <riscv-csr.hpp>

extern "C"
{
  // Per-hart trap nesting depth, maintained by riscv_trap_entry (trap.S).
  extern volatile std::uint32_t riscv_trap_nesting[];
}

namespace riscv
{
  inline unsigned __attribute__ ((always_inline))
  this_hart (void)
  {
#if defined(OS_NCPU) && (OS_NCPU > 1)
    reg_t v;
    __asm__ volatile("csrr %0, mhartid" : "=r"(v));
    return static_cast<unsigned> (v);
#else
    return 0;
#endif
  }

  inline bool __attribute__ ((always_inline))
  in_trap (void)
  {
#if defined(OS_NCPU) && (OS_NCPU > 1)
    if ((RISCV_CSR_READ (mstatus) & csr::MSTATUS_MIE) != 0)
      {
        return false;
      }
#endif
    return riscv_trap_nesting[this_hart ()] != 0;
  }
} // namespace riscv
