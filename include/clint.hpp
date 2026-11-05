/*
 * clint.hpp -- the core-local interruptor: machine timer and machine software
 * interrupt of hart 0.
 *
 * The addresses and the timebase are board facts (riscv-board.hpp). What is
 * an ISA fact is how they are used:
 *
 *   MTIP   the system tick. mtimecmp is compared against the platform timer,
 *          which the hart reads with `rdtime`.
 *   MSIP   the context-switch request -- the RISC-V stand-in for Cortex-M's
 *          PendSV. reschedule() raises it; the trap it causes runs the switch
 *          with interrupts masked, after any timer trap has returned.
 *
 * The Milk-V Duo's CLINT (T-Head C906) cannot be written with a 64-bit store
 * (the bus splits it and the comparator sees a torn value) and its mtime
 * register is not readable. The board says so with MTIMECMP_SPLIT32, and the
 * time comes from `rdtime`, which reads the same counter.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

#include <cstdint>

#include <riscv-board.hpp>
#include <riscv-csr.hpp>

namespace clint
{
  inline volatile std::uint32_t&
  reg32 (std::uintptr_t addr)
  {
    return *reinterpret_cast<volatile std::uint32_t*> (addr);
  }

  inline void
  mtimecmp_write (std::uint64_t value)
  {
    if constexpr (riscv_board::MTIMECMP_SPLIT32)
      {
        // HI to all-ones first, so the comparator never sees a value lower
        // than both the old and the new one while the halves change.
        reg32 (riscv_board::CLINT_MTIMECMP + 4) = 0xFFFFFFFFu;
        reg32 (riscv_board::CLINT_MTIMECMP) = static_cast<std::uint32_t> (value);
        reg32 (riscv_board::CLINT_MTIMECMP + 4)
            = static_cast<std::uint32_t> (value >> 32);
      }
    else
      {
        *reinterpret_cast<volatile std::uint64_t*> (riscv_board::CLINT_MTIMECMP)
            = value;
      }
  }

  inline void
  timer_disarm (void)
  {
    mtimecmp_write (~0ULL);
  }

  inline void
  msip_set (void)
  {
    reg32 (riscv_board::CLINT_MSIP) = 1;
    __asm__ volatile("fence o, i" ::: "memory");
  }

  inline void
  msip_clear (void)
  {
    reg32 (riscv_board::CLINT_MSIP) = 0;
    __asm__ volatile("fence o, i" ::: "memory");
  }

  inline std::uint64_t
  now (void)
  {
    return riscv::csr::rdtime ();
  }

  // The CLINT's own mtime. On the C906 it is NOT readable with a 64-bit load
  // (the access faults); OpenSBI reads it as two 32-bit halves
  // (lib/utils/sys/clint.c, clint_time_rd32), and so does this.
  inline std::uint64_t
  mtime_read (void)
  {
    if constexpr (riscv_board::MTIMECMP_SPLIT32)
      {
        std::uint32_t hi, lo, hi2;
        do
          {
            hi = reg32 (riscv_board::CLINT_MTIME + 4);
            lo = reg32 (riscv_board::CLINT_MTIME);
            hi2 = reg32 (riscv_board::CLINT_MTIME + 4);
          }
        while (hi != hi2);
        return (static_cast<std::uint64_t> (hi) << 32) | lo;
      }
    else
      {
        return *reinterpret_cast<volatile std::uint64_t*> (
            riscv_board::CLINT_MTIME);
      }
  }

  // mtime - rdtime. mtimecmp is compared against mtime, the port counts in
  // rdtime, so a deadline is written as (rdtime deadline + offset). On both
  // boards so far the two are the same counter and the offset is 0, but it
  // is measured, not assumed: the cv1800 bring-up suspected otherwise.
  inline std::int64_t
  measure_offset (void)
  {
    const std::uint64_t t0 = now ();
    const std::uint64_t m = mtime_read ();
    const std::uint64_t t1 = now ();
    return static_cast<std::int64_t> (m - (t0 + (t1 - t0) / 2));
  }
} // namespace clint
