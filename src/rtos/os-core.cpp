/*
 * os-core.cpp -- the port's half of the µOS++ III scheduler, RISC-V RV64,
 * machine mode, one hart.
 *
 * The shape is the upstream Cortex-M port's, translated:
 *
 *   Cortex-M                      this port
 *   ---------------------------   ------------------------------------------
 *   PendSV                        machine software interrupt (CLINT MSIP)
 *   SysTick                       machine timer (CLINT mtimecmp vs rdtime)
 *   PRIMASK                       mstatus.MIE
 *   exception frame + r4-r11      full frame built by trap.S (port_ctx.hpp)
 *   IPSR != 0                     riscv_trap_nesting != 0
 *
 * reschedule() never switches in place. It raises MSIP; the trap that follows
 * (immediately, or as soon as interrupts are unmasked / the current trap
 * returns) saves the running thread, calls switch_stacks() and resumes from
 * the incoming thread's frame. Every switch takes the same path, so the frame
 * of a thread that is not running is always one trap.S wrote or one
 * context::create() built in exactly that layout.
 *
 * Copyright (c) Liviu Ionescu (upstream Cortex-M port, structure).
 * Copyright (c) 2026 Dan. MIT license.
 */

#include <cstring>
#include <cassert>
#include <cstddef>

#include <cmsis-plus/rtos/os.h>
#include <cmsis-plus/rtos/os-c-decls.h>

#include <clint.hpp>
#include <port_ctx.hpp>
#include <riscv-board.hpp>
#include <riscv-csr.hpp>

extern "C"
{
  // The interrupt stack (the board's linker script).
  extern os::rtos::port::stack::element_t __irq_stack_bottom[];
  extern os::rtos::port::stack::element_t __irq_stack_top[];

  // The initial global pointer every thread starts with (linker script).
  extern char __global_pointer$[];

  volatile std::uint64_t riscv_port_last_tick = 0;
  // mtime - rdtime, measured once when the tick starts (clint.hpp).
  volatile std::int64_t riscv_port_clint_offset = 0;
  void riscv_port_tick_rearm (void);
}

namespace
{
  constexpr std::uint64_t tick_period
      = riscv_board::TIMEBASE_HZ / OS_INTEGER_SYSTICK_FREQUENCY_HZ;

  std::uint64_t next_tick = 0;
} // namespace

// Re-arm the comparator one period after the previous deadline, so the tick
// does not drift with interrupt latency. If the hart fell more than a period
// behind (a debugger halt, a long critical section), restart the grid from
// now rather than firing a burst of catch-up ticks.
void
riscv_port_tick_rearm (void)
{
  const std::uint64_t now = clint::now ();
  riscv_port_last_tick = next_tick;
  next_tick += tick_period;
  if (next_tick <= now)
    {
      riscv_port_last_tick = now;
      next_tick = now + tick_period;
    }
  clint::mtimecmp_write (next_tick
                         + static_cast<std::uint64_t> (riscv_port_clint_offset));
}

namespace os
{
  namespace rtos
  {
    namespace port
    {
      // ----------------------------------------------------------------------

      void
      context::create (void* context, void* func, void* args)
      {
        class rtos::thread::context* th_ctx
            = static_cast<class rtos::thread::context*> (context);

        class rtos::thread::stack& stack = th_ctx->stack ();

        // The psABI wants sp 16-byte aligned at every call boundary; the
        // frame size is a multiple of 16, so aligning the top is enough.
        std::uintptr_t top = reinterpret_cast<std::uintptr_t> (stack.top ());
        top &= ~static_cast<std::uintptr_t> (15);
        top -= ctx_size_bytes;

        assert (top > reinterpret_cast<std::uintptr_t> (stack.bottom ())
                          + ctx_size_bytes);

        ctx_t* f = reinterpret_cast<ctx_t*> (top);
        std::memset (f, 0, sizeof (ctx_t));

        // Recognisable garbage in the registers a thread must not rely on.
        for (unsigned i = 5; i < 32; ++i)
          {
            f->x[i] = 0xCCCCCCCC00000000ULL | i;
          }

        f->x[0] = reinterpret_cast<std::uint64_t> (func); // mepc
        f->x[CTX_A0] = reinterpret_cast<std::uint64_t> (args);
        // The thread function never returns (the kernel's trampoline exits
        // the thread); a zero ra makes a return that does happen fault at 0.
        f->x[CTX_RA] = 0;
        f->x[CTX_GP] = reinterpret_cast<std::uint64_t> (__global_pointer$);
        f->x[CTX_TP] = 0;

        // Resume in M-mode, interrupts enabled after mret, FPU on.
        f->mstatus = riscv::csr::MSTATUS_MPP_M | riscv::csr::MSTATUS_MPIE
                     | riscv::csr::MSTATUS_FS_INITIAL;
        f->fcsr = 0;

        th_ctx->port_.stack_ptr
            = reinterpret_cast<rtos::thread::stack::element_t*> (f);
      }

      // ----------------------------------------------------------------------

      void
      clock_systick::start (void)
      {
        riscv_port_clint_offset = clint::measure_offset ();
        next_tick = clint::now ();
        riscv_port_tick_rearm ();
        RISCV_CSR_SET (mie, riscv::csr::MIE_MTIE);
      }

      // ----------------------------------------------------------------------

      namespace scheduler
      {
        state_t lock_state;

        result_t
        initialize (void)
        {
          riscv::csr::mstatus_clear_mie ();
          return result::ok;
        }

        void
        start (void)
        {
          riscv::csr::mstatus_clear_mie ();

          // The interrupt stack: a linker-script region of its own, on which
          // trap.S runs the dispatcher. Registered here, not at start-up: the
          // stack object is a static whose constructor runs with the other
          // static constructors and would reset an earlier set() -- which is
          // also why upstream's Cortex-M port does it in start(). The guard
          // pattern is written now, before any trap has used the stack.
          rtos::interrupts::stack ()->set (
              __irq_stack_bottom,
              static_cast<std::size_t> (
                  reinterpret_cast<char*> (__irq_stack_top)
                  - reinterpret_cast<char*> (__irq_stack_bottom)));
          rtos::interrupts::stack ()->initialize ();

          // A thread to save the boot context into. Its frame lands on the
          // boot stack and is never resumed.
          static os_thread_t fake_thread;
          std::memset (&fake_thread, 0, sizeof (os_thread_t));
          fake_thread.name = "fake_thread";
          rtos::scheduler::current_thread_
              = reinterpret_cast<rtos::thread*> (&fake_thread);

          lock_state = state::init;

          clint::msip_clear ();
          RISCV_CSR_SET (mie, riscv::csr::MIE_MSIE);

          // Request the first switch, then let it happen.
          reschedule ();
          riscv::csr::mstatus_set_mie ();

          for (;;)
            {
              __asm__ volatile("wfi");
            }
        }

        state_t
        locked (state_t state)
        {
          os_assert_throw (!interrupts::in_handler_mode (), EPERM);

          state_t tmp;
          {
            rtos::interrupts::critical_section ics;
            tmp = lock_state;
            lock_state = state;
          }
          // Unlocking may have left a switch pending behind the lock.
          if (tmp == state::locked && state == state::unlocked
              && rtos::scheduler::is_reschedule_pending)
            {
              clint::msip_set ();
            }
          return tmp;
        }

        void
        reschedule (void)
        {
          if (rtos::scheduler::locked ()
              || (rtos::interrupts::in_handler_mode ()
                  && !rtos::scheduler::preemptive ()))
            {
              return;
            }

          rtos::scheduler::is_reschedule_pending = true;
          clint::msip_set ();
        }

        stack::element_t*
        switch_stacks (stack::element_t* sp)
        {
          // Runs in the MSIP trap, interrupts masked.
          clint::msip_clear ();
          rtos::scheduler::is_reschedule_pending = false;

          rtos::thread* old_thread = rtos::scheduler::current_thread_;
          old_thread->context_.port_.stack_ptr = sp;

          rtos::scheduler::internal_switch_threads ();

          return rtos::scheduler::current_thread_->context_.port_.stack_ptr;
        }
      } /* namespace scheduler */

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */
