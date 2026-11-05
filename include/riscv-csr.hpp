/*
 * riscv-csr.hpp -- machine-mode CSR access for the RISC-V port.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

#include <cstdint>

namespace riscv
{
  namespace csr
  {
    // mstatus
    inline constexpr std::uint64_t MSTATUS_MIE = 1ULL << 3;
    inline constexpr std::uint64_t MSTATUS_MPIE = 1ULL << 7;
    inline constexpr std::uint64_t MSTATUS_MPP_M = 3ULL << 11;
    inline constexpr std::uint64_t MSTATUS_MPP_S = 1ULL << 11;
    inline constexpr std::uint64_t MSTATUS_MPP_MASK = 3ULL << 11;
    inline constexpr std::uint64_t MSTATUS_FS_INITIAL = 1ULL << 13;
    inline constexpr std::uint64_t MSTATUS_FS_MASK = 3ULL << 13;
    inline constexpr std::uint64_t MSTATUS_MPRV = 1ULL << 17;

    // mie / mip
    inline constexpr std::uint64_t MIE_MSIE = 1ULL << 3;
    inline constexpr std::uint64_t MIE_MTIE = 1ULL << 7;
    inline constexpr std::uint64_t MIE_MEIE = 1ULL << 11;

    // mcause
    inline constexpr std::uint64_t MCAUSE_INTERRUPT = 1ULL << 63;
    inline constexpr std::uint64_t IRQ_M_SOFT = 3;
    inline constexpr std::uint64_t IRQ_M_TIMER = 7;
    inline constexpr std::uint64_t IRQ_M_EXT = 11;
    inline constexpr std::uint64_t EXC_ILLEGAL = 2;
    inline constexpr std::uint64_t EXC_BREAKPOINT = 3;
    inline constexpr std::uint64_t EXC_ECALL_M = 11;

#define RISCV_CSR_READ(name)                                                  \
  ({                                                                          \
    std::uint64_t _v;                                                         \
    __asm__ volatile("csrr %0, " #name : "=r"(_v)::"memory");                 \
    _v;                                                                       \
  })
#define RISCV_CSR_WRITE(name, v)                                              \
  __asm__ volatile("csrw " #name ", %0" ::"r"((std::uint64_t) (v)) : "memory")
#define RISCV_CSR_SET(name, v)                                                \
  __asm__ volatile("csrs " #name ", %0" ::"r"((std::uint64_t) (v)) : "memory")
#define RISCV_CSR_CLEAR(name, v)                                              \
  __asm__ volatile("csrc " #name ", %0" ::"r"((std::uint64_t) (v)) : "memory")

    // Clear mstatus.MIE and return the previous MIE bit (0 or MSTATUS_MIE).
    inline std::uint64_t __attribute__ ((always_inline))
    mstatus_clear_mie (void)
    {
      std::uint64_t v;
      __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(v)::"memory");
      return v & MSTATUS_MIE;
    }

    // Set mstatus.MIE and return the previous MIE bit.
    inline std::uint64_t __attribute__ ((always_inline))
    mstatus_set_mie (void)
    {
      std::uint64_t v;
      __asm__ volatile("csrrsi %0, mstatus, 8" : "=r"(v)::"memory");
      return v & MSTATUS_MIE;
    }

    // The platform timer (time CSR). The C906 and QEMU's virt machine both
    // implement it in M-mode.
    inline std::uint64_t __attribute__ ((always_inline))
    rdtime (void)
    {
      std::uint64_t v;
      __asm__ volatile("rdtime %0" : "=r"(v));
      return v;
    }

    inline std::uint64_t __attribute__ ((always_inline))
    rdcycle (void)
    {
      std::uint64_t v;
      __asm__ volatile("rdcycle %0" : "=r"(v));
      return v;
    }
  } // namespace csr
} // namespace riscv
