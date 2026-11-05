/*
 * os-core.cpp -- the port's half of the µOS++ III scheduler, RISC-V RV64,
 * machine mode, one hart.
 *
 * RISC-V has no PendSV. The context switch uses what RISC-V guarantees:
 *
 *   a thread switches     reschedule() executes `ecall` (a7 = the port's
 *                         marker). An exception is precise and synchronous:
 *                         the trap is taken AT the ecall, so a thread that
 *                         has just marked itself suspended cannot run one
 *                         more instruction before it is saved. trap.cpp
 *                         calls switch_stacks() and mrets on the incoming
 *                         thread's frame; the caller continues after the
 *                         ecall when it is switched back in.
 *   a trap makes a thread due (tick, post from an interrupt)
 *                         reschedule() only sets is_reschedule_pending;
 *                         riscv_trap_dispatch() switches on the way out of
 *                         the outermost trap. No second interrupt.
 *   asked with MIE clear  (inside a critical section) deferred:
 *                         critical_section::exit() switches when it sets
 *                         MIE again -- what clearing PRIMASK does for a
 *                         pending PendSV.
 *   scheduler locked      nothing; unlocking switches if one is pending.
 *
 * A CLINT MSIP write is a posted store: the C906 takes the interrupt some
 * cycles after it (QEMU at once). An MSIP-based "PendSV" therefore let a
 * suspending thread run past reschedule() and remove its own wake-up --
 * measured on the Duo. MSIP now carries only the software interrupt lines
 * (soft-irq.cpp).
 *
 *   SysTick       machine timer (CLINT mtimecmp vs rdtime)
 *   PRIMASK       mstatus.MIE
 *   frame         full frame built by trap.S (port_ctx.hpp)
 *   IPSR != 0     riscv_trap_nesting != 0
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
  void riscv_port_tick_guard (void);

  volatile bool riscv_port_yield_deferred = false;
  void riscv_port_yield (void);
  void riscv_port_deferred_yield (void);
}

// The thread-mode context switch: a synchronous `ecall` (mcause 11) with
// a7 = RISCV_PORT_YIELD_ECALL. The exception is precise -- taken AT this
// instruction, with interrupts on or off -- so the calling thread does not
// execute one more instruction before trap.cpp has saved it and resumed the
// next thread. It continues after the ecall only when switched back in.
// All registers are saved and restored by trap.S; nothing is clobbered.
void
riscv_port_yield (void)
{
  register ctx_reg_t a7 __asm__ ("a7") = RISCV_PORT_YIELD_ECALL;
  // a7 comes back 0 (trap.cpp consumes the marker).
  __asm__ volatile("ecall" : "+r"(a7) : : "memory");
}

// A switch asked for while it could not happen (MIE clear, or the scheduler
// locked): do it now, if it can happen now.
void
riscv_port_deferred_yield (void)
{
  if (os::rtos::interrupts::in_handler_mode ()
      || os::rtos::scheduler::locked ()
      || (RISCV_CSR_READ (mstatus) & riscv::csr::MSTATUS_MIE) == 0)
    {
      return;
    }
  riscv_port_yield ();
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

// Called AFTER the tick handler. If the handler itself ran past the next
// deadline (a timer callback that is slow, e.g. a semihosting write, which
// halts the hart for a JTAG round trip), the next tick would be taken right
// after mret and no thread would ever run again -- measured on the Duo with
// rtos-apis' 1-tick periodic timer: the hart lived in the tick trap. So the
// next deadline is moved to one full period from now: threads always get
// at least one period between ticks. The ticks lost are lost, as after any
// halt.
void
riscv_port_tick_guard (void)
{
  const std::uint64_t now = clint::now ();
  if (next_tick <= now)
    {
      next_tick = now + tick_period;
      clint::mtimecmp_write (
          next_tick + static_cast<std::uint64_t> (riscv_port_clint_offset));
    }
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
            f->x[i] = static_cast<ctx_reg_t> (0xCCCCCCCC00000000ULL) | i;
          }

        f->x[0] = reinterpret_cast<std::uintptr_t> (func); // mepc
        f->x[CTX_A0] = reinterpret_cast<std::uintptr_t> (args);
        // The thread function never returns (the kernel's trampoline exits
        // the thread); a zero ra makes a return that does happen fault at 0.
        f->x[CTX_RA] = 0;
        f->x[CTX_GP] = reinterpret_cast<std::uintptr_t> (__global_pointer$);
        f->x[CTX_TP] = 0;

        // Resume in M-mode, interrupts enabled after mret; FPU on, if any.
        f->mstatus = riscv::csr::MSTATUS_MPP_M | riscv::csr::MSTATUS_MPIE;
#if defined(__riscv_flen)
        f->mstatus |= riscv::csr::MSTATUS_FS_INITIAL;
        f->fcsr = 0;
#endif

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

          // MSIP is left to the software interrupt lines (soft-irq.cpp).
          clint::msip_clear ();
          RISCV_CSR_SET (mie, riscv::csr::MIE_MSIE);

          // The first switch, synchronously. The boot context is saved into
          // fake_thread (with MIE clear) and never resumed; the first
          // thread's frame (context::create) enables interrupts on mret.
          riscv_port_yield ();

          for (;;)
            {
              __asm__ volatile("" ::: "memory");
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
              && (rtos::scheduler::is_reschedule_pending
                  || riscv_port_yield_deferred))
            {
              riscv_port_deferred_yield ();
            }
          return tmp;
        }

        void
        reschedule (void)
        {
          if (rtos::scheduler::locked ())
            {
              return;
            }

          if (rtos::interrupts::in_handler_mode ())
            {
              // From a trap: the switch is done by riscv_trap_dispatch()
              // on the way out of the outermost trap.
              if (rtos::scheduler::preemptive ())
                {
                  rtos::scheduler::is_reschedule_pending = true;
                }
              return;
            }

          if ((RISCV_CSR_READ (mstatus) & riscv::csr::MSTATUS_MIE) == 0)
            {
              // Inside a critical section: switching now would let another
              // thread run inside it. critical_section::exit() does it.
              riscv_port_yield_deferred = true;
              return;
            }

          riscv_port_yield ();
        }

        stack::element_t*
        switch_stacks (stack::element_t* sp)
        {
          // Runs in a trap, interrupts masked.
          rtos::scheduler::is_reschedule_pending = false;
          riscv_port_yield_deferred = false;

          rtos::thread* old_thread = rtos::scheduler::current_thread_;
          old_thread->context_.port_.stack_ptr = sp;

          rtos::scheduler::internal_switch_threads ();

          return rtos::scheduler::current_thread_->context_.port_.stack_ptr;
        }
      } /* namespace scheduler */

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */
