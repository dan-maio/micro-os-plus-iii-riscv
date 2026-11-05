/*
 * port_ctx.hpp -- the saved-context frame the RISC-V trap entry builds.
 *
 * Shared by src/trap.S (which writes and reads it by offset) and by
 * src/rtos/os-core.cpp (which builds the first frame of every thread). The
 * offsets below are the ones trap.S uses; the static_asserts keep the two
 * from drifting apart.
 *
 * Every trap saves the full integer and floating-point register files on the
 * stack it interrupted. A context switch is therefore "return from the trap
 * on a different stack": the scheduler only swaps the saved stack pointer.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#pragma once

#if !defined(__ASSEMBLER__)
#include <cstddef>
#include <cstdint>
#endif

// Byte offsets, also used by the assembler (trap.S includes this header
// under __ASSEMBLER__ for the numbers only).
#define PORT_CTX_X(n) ((n) * 8)     // x1..x31 at their own index; x0 slot = mepc
#define PORT_CTX_MEPC 0
#define PORT_CTX_MSTATUS (32 * 8)   // 256
#define PORT_CTX_FCSR (33 * 8)      // 264
#define PORT_CTX_F(n) (34 * 8 + (n) * 8) // 272 + 8n
#define PORT_CTX_SIZE (66 * 8)      // 528, a multiple of 16

#if !defined(__ASSEMBLER__)

struct ctx_t
{
  // x[0] holds mepc (x0 is hard-wired zero and needs no slot); x[2], the
  // stack pointer, is implicit -- it is the frame address plus the frame size.
  std::uint64_t x[32];
  std::uint64_t mstatus;
  std::uint64_t fcsr;
  std::uint64_t f[32];
};

static_assert (offsetof (ctx_t, mstatus) == PORT_CTX_MSTATUS);
static_assert (offsetof (ctx_t, fcsr) == PORT_CTX_FCSR);
static_assert (offsetof (ctx_t, f) == PORT_CTX_F (0));
static_assert (sizeof (ctx_t) == PORT_CTX_SIZE);
static_assert ((PORT_CTX_SIZE % 16) == 0, "the psABI wants 16-byte stacks");

inline constexpr std::size_t ctx_size_bytes = sizeof (ctx_t);

// Register numbers used when building a frame.
inline constexpr unsigned CTX_RA = 1;
inline constexpr unsigned CTX_GP = 3;
inline constexpr unsigned CTX_TP = 4;
inline constexpr unsigned CTX_A0 = 10;
inline constexpr unsigned CTX_A1 = 11;
inline constexpr unsigned CTX_A7 = 17;

// a7 of the port's own ecall (os-core.cpp riscv_port_yield): "switch
// threads". Any other ecall is an application's (riscv_trap_exception_hook).
inline constexpr std::uint64_t RISCV_PORT_YIELD_ECALL = 0x55'4F'53'59ULL; // "YSOU"

#endif
