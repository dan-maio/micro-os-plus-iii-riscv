/*
 * trap.cpp -- the C half of the RISC-V trap path (see trap.S).
 *
 *   ecall, a7 = RISCV_PORT_YIELD_ECALL  a thread's context switch: return
 *                                       the incoming thread's frame
 *   machine software interrupt (MSIP)   the software interrupt lines
 *   machine timer interrupt (MTIP)      the system tick
 *   machine external interrupt (MEIP)   the board (PLIC), if it wants it
 *   breakpoint at a semihosting trap    no debugger: skip it, a0 = -1
 *   any other exception                 a fault report, then stop
 *
 * Leaving the outermost trap, a reschedule left pending by the handler
 * (a woken thread) switches threads there.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */

#include <cstdint>
#include <cstddef>

#include <cmsis-plus/rtos/os.h>

#include <clint.hpp>
#include <port_ctx.hpp>
#include <riscv-board.hpp>
#include <riscv-csr.hpp>
#include <semihosting.hpp>

extern "C"
{
  void os_systick_handler (void);

  volatile bool riscv_semihosting_absent = false;

  // Defined in os-core.cpp: re-arm mtimecmp for the next tick, and push it
  // out again if the handler overran it.
  void riscv_port_tick_rearm (void);
  void riscv_port_tick_guard (void);

  // soft-irq.cpp: run the pending, enabled software interrupt lines.
  void riscv_soft_irq_dispatch (void);

  void __attribute__ ((weak)) riscv_board_external_interrupt (void)
  {
  }

  // An exception a test (or an application) wants to handle itself: return
  // true after adjusting the frame (typically frame->x[0], mepc, past the
  // faulting instruction); false lets the port report it as a fault.
  // exc-test is the user: it provokes illegal-instruction, ecall and
  // misaligned/access exceptions and proves the trap entry survives them.
  bool __attribute__ ((weak)) riscv_trap_exception_hook (ctx_t* frame,
                                                         std::uint64_t mcause,
                                                         std::uint64_t mtval)
  {
    (void) frame;
    (void) mcause;
    (void) mtval;
    return false;
  }

  void* riscv_trap_dispatch (ctx_t* frame);
}

namespace
{
  // Raw console output: no locks, no scheduler. Usable from a fault.
  void
  raw (const char* s)
  {
    std::size_t n = 0;
    while (s[n] != '\0')
      {
        ++n;
      }
    riscv_board_console_write (s, n);
  }

  void
  raw_hex (std::uint64_t v)
  {
    char b[19];
    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 16; ++i)
      {
        const unsigned d = static_cast<unsigned> ((v >> (60 - 4 * i)) & 0xF);
        b[2 + i] = static_cast<char> (d < 10 ? '0' + d : 'a' + d - 10);
      }
    b[18] = '\0';
    raw (b);
  }

  bool
  is_semihosting_trap (std::uint64_t pc)
  {
    // The sequence is uncompressed and 4-byte aligned (semihosting.hpp).
    if ((pc & 3) != 0)
      {
        return false;
      }
    const auto* p = reinterpret_cast<const std::uint32_t*> (pc);
    return p[-1] == 0x01f01013u     // slli x0, x0, 0x1f
           && p[0] == 0x00100073u   // ebreak
           && p[1] == 0x40705013u;  // srai x0, x0, 7
  }

  [[noreturn]] void
  fault (ctx_t* frame, std::uint64_t mcause)
  {
    raw ("\r\n!!! RISC-V trap: mcause=");
    raw_hex (mcause);
    raw (" mepc=");
    raw_hex (frame->x[0]);
    raw (" mtval=");
    raw_hex (RISCV_CSR_READ (mtval));
    raw ("\r\n    ra=");
    raw_hex (frame->x[CTX_RA]);
    raw (" sp=");
    raw_hex (reinterpret_cast<std::uint64_t> (frame) + ctx_size_bytes);
    raw ("\r\nRESULT: FAIL (trap)\r\n");
    // The verdict on the semihosting channel, then SYS_EXIT stops the run
    // (without a debugger, exit_failure() spins).
    semihosting::write_str ("RESULT: FAIL (trap)\n");
    semihosting::exit_failure ();
  }
} // namespace

namespace
{
  // Leaving the outermost trap: if the trap made another thread due (a tick,
  // a post from an interrupt), resume that thread instead -- the job PendSV
  // tail-chaining does on Cortex-M, done here without a second interrupt.
  // A nested trap (an exception inside a handler) never switches.
  void*
  trap_exit (ctx_t* frame)
  {
    if (riscv_trap_nesting == 1 && os::rtos::scheduler::is_reschedule_pending
        && !os::rtos::scheduler::locked ())
      {
        return os::rtos::port::scheduler::switch_stacks (
            reinterpret_cast<os::rtos::port::stack::element_t*> (frame));
      }
    return frame;
  }
} // namespace

void*
riscv_trap_dispatch (ctx_t* frame)
{
  const std::uint64_t mcause = RISCV_CSR_READ (mcause);

  if (mcause & riscv::csr::MCAUSE_INTERRUPT)
    {
      // The C906 may set vendor bits above the code: compare the code only.
      switch (mcause & 0xFFF)
        {
        case riscv::csr::IRQ_M_SOFT:
          // MSIP: the software interrupt lines (soft-irq.cpp) only.
          clint::msip_clear ();
          riscv_soft_irq_dispatch ();
          return trap_exit (frame);

        case riscv::csr::IRQ_M_TIMER:
          riscv_port_tick_rearm ();
          os_systick_handler ();
          riscv_port_tick_guard ();
          return trap_exit (frame);

        case riscv::csr::IRQ_M_EXT:
          riscv_board_external_interrupt ();
          return trap_exit (frame);

        default:
          fault (frame, mcause);
        }
    }

  if (mcause == riscv::csr::EXC_ECALL_M
      && frame->x[CTX_A7] == RISCV_PORT_YIELD_ECALL
      && riscv_trap_nesting == 1)
    {
      // A thread's context switch (os-core.cpp riscv_port_yield). The
      // thread resumes after the 4-byte ecall when it is switched back in.
      // The marker is consumed: a later, unrelated ecall of this thread
      // must not find it still in a7.
      frame->x[0] += 4;
      frame->x[CTX_A7] = 0;
      return os::rtos::port::scheduler::switch_stacks (
          reinterpret_cast<os::rtos::port::stack::element_t*> (frame));
    }

  if (mcause == riscv::csr::EXC_BREAKPOINT && is_semihosting_trap (frame->x[0]))
    {
      // No debugger behind the semihosting call: answer -1 and step over
      // the ebreak and the srai that follows it.
      riscv_semihosting_absent = true;
      frame->x[CTX_A0] = static_cast<ctx_reg_t> (-1);
      frame->x[0] += 8;
      return frame;
    }

  if (riscv_trap_exception_hook (frame, mcause, RISCV_CSR_READ (mtval)))
    {
      return frame;
    }

  fault (frame, mcause);
}
