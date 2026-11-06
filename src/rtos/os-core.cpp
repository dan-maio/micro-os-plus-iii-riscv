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
#include <riscv-amo.hpp>
#include <riscv-board.hpp>
#include <riscv-csr.hpp>

#if !defined(OS_NCPU)
#define OS_NCPU 1
#endif

extern "C"
{
  // The interrupt stack of hart 0 (the board's linker script).
  extern os::rtos::port::stack::element_t __irq_stack_bottom[];
  extern os::rtos::port::stack::element_t __irq_stack_top[];

  // The initial global pointer every thread starts with (linker script).
  extern char __global_pointer$[];

  volatile std::uint64_t riscv_port_last_tick = 0;
  // mtime - rdtime, measured once when the tick starts (clint.hpp).
  volatile std::int64_t riscv_port_clint_offset = 0;
  void riscv_port_tick_rearm (void);
  void riscv_port_tick_guard (void);

  // Per hart.
  volatile bool riscv_port_yield_deferred[OS_NCPU];
  volatile bool riscv_port_pending[OS_NCPU];
  void riscv_port_yield (void);
  void riscv_port_deferred_yield (void);

  // Per hart: the top of its interrupt stack, read by trap.S on the
  // outermost trap. Hart 0's is the linker's, a constant initialiser, so a
  // trap during start-up has it; the others' are set in scheduler::start(),
  // before any secondary hart can trap.
  os::rtos::port::stack::element_t* riscv_irq_stack_top[OS_NCPU]
      = { __irq_stack_top };

  // The trap glue (trap.cpp).
  void riscv_port_tick (void);
  void riscv_port_ipi_received (void);
  bool riscv_port_switch_due (void);

#if defined(OS_USE_SMP_SCHEDULER)
  // The first switches of the run (old != new), for a debugger (written
  // under the kernel spinlock): which hart, the outgoing and the incoming
  // thread, and the state the outgoing one had when it was switched out.
  struct riscv_sw_rec
  {
    std::uint32_t hart;
    std::uint32_t old_state;
    void* old_thread;
    void* new_thread;
  };
  volatile riscv_sw_rec riscv_sw_log[256];
  volatile std::uint32_t riscv_sw_count;

  unsigned port_cpu_id (void);
  void port_smp_ipi (unsigned cpu);
  void port_smp_secondary_start (void);
  std::uintptr_t port_core1_stack_top (void);
  void port_smp_core0_ipi_enable (void);
  void riscv_smp_klock_acquire (void);
  void riscv_smp_klock_release (void);
#endif
}

// The kernel's idle thread (os-idle.cpp) and, on SMP, the per-hart idle
// array (os-core.cpp); neither is in a public header (as in the RP2350
// Cortex-M port).
extern os::rtos::thread* os_idle_thread;
#if defined(OS_USE_SMP_SCHEDULER)
namespace os::rtos::scheduler
{
  extern thread* os_idle_thread_core[OS_NCPU];
}
#endif

namespace
{
#if defined(OS_USE_SMP_SCHEDULER)
  // The interrupt stacks of harts 1..OS_NCPU-1 and their first stacks (the
  // one port_smp_secondary_start() runs on until its first switch; the
  // fake thread's frame lands there and is never resumed).
  constexpr std::size_t secondary_irq_stack_words = 8192 / sizeof (std::uintptr_t);
  constexpr std::size_t secondary_boot_stack_words = 2048 / sizeof (std::uintptr_t);
  alignas (16) std::uintptr_t
      secondary_irq_stack[OS_NCPU - 1][secondary_irq_stack_words];
  alignas (16) std::uintptr_t
      secondary_boot_stack[OS_NCPU - 1][secondary_boot_stack_words];
#endif
} // namespace

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

  // Per hart: each hart has its own comparator and its own grid.
  std::uint64_t next_tick[OS_NCPU];

  void
  tick_write (unsigned hart)
  {
    clint::mtimecmp_write (next_tick[hart]
                               + static_cast<std::uint64_t> (
                                   riscv_port_clint_offset),
                           hart);
  }

  void
  tick_start_this_hart (void)
  {
    const unsigned h = riscv::this_hart ();
    next_tick[h] = clint::now ();
    riscv_port_tick_rearm ();
    RISCV_CSR_SET (mie, riscv::csr::MIE_MTIE);
  }
} // namespace

// Re-arm this hart's comparator one period after its previous deadline, so
// the tick does not drift with interrupt latency. If the hart fell more than
// a period behind (a debugger halt, a long critical section), restart the
// grid from now rather than firing a burst of catch-up ticks. Hart 0's tick
// is the system clock: only it moves riscv_port_last_tick (the high-
// resolution clock's base).
void
riscv_port_tick_rearm (void)
{
  const unsigned h = riscv::this_hart ();
  const std::uint64_t now = clint::now ();
  std::uint64_t last = next_tick[h];
  next_tick[h] += tick_period;
  if (next_tick[h] <= now)
    {
      last = now;
      next_tick[h] = now + tick_period;
    }
  if (h == 0)
    {
      riscv_port_last_tick = last;
    }
  tick_write (h);
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
  const unsigned h = riscv::this_hart ();
  const std::uint64_t now = clint::now ();
  if (next_tick[h] <= now)
    {
      next_tick[h] = now + tick_period;
      tick_write (h);
    }
}

extern "C" void os_systick_handler (void);

// The machine-timer trap. Hart 0 advances the RTOS clock (which ends by
// rescheduling); another hart's tick only reschedules that hart, so it picks
// up threads hart 0 has woken -- as core 1's SysTick does in the RP2350
// Cortex-M port.
void
riscv_port_tick (void)
{
  riscv_port_tick_rearm ();
  if (riscv::this_hart () == 0)
    {
      os_systick_handler ();
    }
  else
    {
      os::rtos::port::scheduler::reschedule ();
    }
  riscv_port_tick_guard ();
}

// The machine-software-interrupt trap, after the software IRQ lines ran:
// on SMP it is also the cross-hart "reschedule" request (port_smp_ipi).
void
riscv_port_ipi_received (void)
{
#if defined(OS_USE_SMP_SCHEDULER)
  if (os::rtos::scheduler::started ())
    {
      os::rtos::port::scheduler::reschedule ();
    }
#endif
}

// Leaving the outermost trap of this hart: is a switch due?
bool
riscv_port_switch_due (void)
{
  const unsigned h = riscv::this_hart ();
  if (!riscv_port_pending[h])
    {
      return false;
    }
#if defined(OS_USE_SMP_SCHEDULER)
  return os::rtos::port::scheduler::lock_state[h]
         == os::rtos::port::scheduler::state::unlocked;
#else
  return os::rtos::port::scheduler::lock_state
         == os::rtos::port::scheduler::state::unlocked;
#endif
}

#if defined(OS_USE_SMP_SCHEDULER)
// ----------------------------------------------------------------------------
// The kernel spinlock: recursive (owner + depth), taken with IRQs masked on
// this hart. The word is an AMO swap (amoswap.w.aq / store-release), which
// both the QEMU harts and the RP2350's Hazard3 cores implement (the A
// extension; the RP2350's global monitor makes it atomic across cores).

void
riscv_smp_klock_acquire (void)
{
  using os::rtos::port::scheduler::smp_klock;
  const unsigned h = riscv::this_hart ();
  if (smp_klock.owner == h)
    {
      smp_klock.depth = smp_klock.depth + 1;
      return;
    }
  while (__atomic_exchange_n (&smp_klock.word, 1u, __ATOMIC_ACQUIRE) != 0)
    {
      while (__atomic_load_n (&smp_klock.word, __ATOMIC_RELAXED) != 0)
        {
        }
    }
  smp_klock.owner = h;
  smp_klock.depth = 1;
}

void
riscv_smp_klock_release (void)
{
  using os::rtos::port::scheduler::smp_klock;
  if (smp_klock.owner != riscv::this_hart () || smp_klock.depth == 0)
    {
      return;
    }
  const std::uint32_t d = smp_klock.depth - 1;
  smp_klock.depth = d;
  if (d == 0)
    {
      smp_klock.owner = SMP_NO_OWNER;
      __atomic_store_n (&smp_klock.word, 0u, __ATOMIC_RELEASE);
    }
}
#endif
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
        tick_start_this_hart ();
      }

      // ----------------------------------------------------------------------

      namespace scheduler
      {
#if defined(OS_USE_SMP_SCHEDULER)
        volatile state_t lock_state[OS_NCPU];
        // Taken with amoswap: kept where AMOs work (riscv-amo.hpp).
        os_port_smp_klock_t smp_klock RISCV_AMO_DATA = { 0, SMP_NO_OWNER, 0 };
        namespace
        {
          // The MIE state to restore when the scheduler lock is released.
          riscv::reg_t lock_mie[OS_NCPU];
        } // namespace
#else
        state_t lock_state;
#endif

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

#if defined(OS_USE_SMP_SCHEDULER)
          for (unsigned h = 1; h < OS_NCPU; ++h)
            {
              riscv_irq_stack_top[h]
                  = reinterpret_cast<stack::element_t*> (
                      &secondary_irq_stack[h - 1][secondary_irq_stack_words]);
            }
          for (unsigned h = 0; h < OS_NCPU; ++h)
            {
              lock_state[h] = state::init;
              riscv_port_pending[h] = false;
              riscv_port_yield_deferred[h] = false;
            }
          rtos::scheduler::current_thread_[0]
              = reinterpret_cast<rtos::thread*> (&fake_thread);
          rtos::scheduler::os_idle_thread_core[0] = ::os_idle_thread;
#else
          rtos::scheduler::current_thread_
              = reinterpret_cast<rtos::thread*> (&fake_thread);
          lock_state = state::init;
#endif

          // MSIP: the software interrupt lines (soft-irq.cpp) and, on SMP,
          // the cross-hart reschedule request.
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

#if defined(OS_USE_SMP_SCHEDULER)
        // The scheduler lock, per hart. Locking takes the kernel spinlock
        // and keeps this hart's interrupts masked until the unlock, as the
        // RP2350 Cortex-M port does: nothing else may touch the kernel's
        // lists while a thread has the scheduler locked. MIE is cleared
        // BEFORE the hart id is read, so the thread cannot move to the other
        // hart between the read and the write of its lock state.
        state_t
        locked (state_t state)
        {
          os_assert_throw (!interrupts::in_handler_mode (), EPERM);

          const riscv::reg_t s = riscv::csr::mstatus_clear_mie ();
          const unsigned h = riscv::this_hart ();
          const state_t tmp = lock_state[h];

          if (state == tmp)
            {
              if (s)
                {
                  riscv::csr::mstatus_set_mie ();
                }
              return tmp;
            }

          if (state == state::locked)
            {
              riscv_smp_klock_acquire ();
              lock_mie[h] = s;
              lock_state[h] = state::locked;
              return tmp; // interrupts stay masked
            }

          // Unlock: publish "unlocked" before releasing the spinlock, then
          // restore the MIE state the lock was taken with.
          lock_state[h] = state::unlocked;
          riscv_smp_klock_release ();
          const riscv::reg_t mie = lock_mie[h];
          const bool due = riscv_port_pending[h] || riscv_port_yield_deferred[h];
          if (mie)
            {
              riscv::csr::mstatus_set_mie ();
              if (due)
                {
                  riscv_port_yield ();
                }
            }
          return tmp;
        }

        void
        reschedule (void)
        {
          const riscv::reg_t s = riscv::csr::mstatus_clear_mie ();
          const unsigned h = riscv::this_hart ();

          if (lock_state[h] != state::unlocked)
            {
              if (s)
                {
                  riscv::csr::mstatus_set_mie ();
                }
              return;
            }

          if (riscv_trap_nesting[h] != 0)
            {
              // From a trap: the switch is made on the way out of this
              // hart's outermost trap (riscv_port_switch_due()). A nested
              // interrupt handler may run with MIE set: give it back.
              if (rtos::scheduler::preemptive ())
                {
                  riscv_port_pending[h] = true;
                }
              if (s)
                {
                  riscv::csr::mstatus_set_mie ();
                }
              return;
            }

          if (!s)
            {
              // Inside a critical section: switching now would let another
              // thread run inside it. critical_section::exit() does it.
              riscv_port_yield_deferred[h] = true;
              return;
            }

          riscv::csr::mstatus_set_mie ();
          riscv_port_yield ();
        }

        // Runs in a trap on this hart, interrupts masked. The whole switch is
        // under the kernel spinlock, so the other hart cannot touch the ready
        // list meanwhile. The kernel's SMP rule (internal_switch_threads):
        // a thread whose stack_ptr is nullptr is live on some hart and is
        // not picked. So the outgoing frame -- complete on the outgoing
        // thread's stack, trap.S saved it before switching to this hart's
        // interrupt stack -- is published at once, and the incoming thread
        // is claimed by clearing its stack_ptr.
        stack::element_t*
        switch_stacks (stack::element_t* sp)
        {
          const unsigned h = riscv::this_hart ();
          riscv_smp_klock_acquire ();

          riscv_port_pending[h] = false;
          riscv_port_yield_deferred[h] = false;

          rtos::thread* old_thread = rtos::scheduler::current_thread_[h];
          old_thread->context_.port_.stack_ptr = sp;
          const std::uint32_t old_state = old_thread->state ();

          rtos::scheduler::internal_switch_threads ();

          rtos::thread* new_thread = rtos::scheduler::current_thread_[h];
          if (new_thread != old_thread && riscv_sw_count < 256)
            {
            const std::uint32_t i = riscv_sw_count;
            volatile riscv_sw_rec& r = riscv_sw_log[i];
            r.hart = h;
            r.old_state = old_state;
            r.old_thread = old_thread;
            r.new_thread = new_thread;
            riscv_sw_count = i + 1;
          }
          if (new_thread == nullptr)
            {
              // Nothing may run here and there is no idle thread for this
              // hart: a board error. Stay in the trap, masked.
              riscv_smp_klock_release ();
              for (;;)
                {
                  __asm__ volatile("" ::: "memory");
                }
            }

          stack::element_t* out_sp;
          if (new_thread != old_thread)
            {
              out_sp = new_thread->context_.port_.stack_ptr;
              new_thread->context_.port_.stack_ptr = nullptr;
            }
          else
            {
              out_sp = sp;
              old_thread->context_.port_.stack_ptr = nullptr;
            }

          riscv_smp_klock_release ();
          return out_sp;
        }
#else
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
              && (riscv_port_pending[0] || riscv_port_yield_deferred[0]))
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
                  riscv_port_pending[0] = true;
                }
              return;
            }

          if ((RISCV_CSR_READ (mstatus) & riscv::csr::MSTATUS_MIE) == 0)
            {
              // Inside a critical section: switching now would let another
              // thread run inside it. critical_section::exit() does it.
              riscv_port_yield_deferred[0] = true;
              return;
            }

          riscv_port_yield ();
        }

        stack::element_t*
        switch_stacks (stack::element_t* sp)
        {
          // Runs in a trap, interrupts masked.
          riscv_port_pending[0] = false;
          riscv_port_yield_deferred[0] = false;

          rtos::thread* old_thread = rtos::scheduler::current_thread_;
          old_thread->context_.port_.stack_ptr = sp;

          rtos::scheduler::internal_switch_threads ();

          return rtos::scheduler::current_thread_->context_.port_.stack_ptr;
        }
#endif
      } /* namespace scheduler */

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

#if defined(OS_USE_SMP_SCHEDULER)
// ----------------------------------------------------------------------------
// The SMP entry points the kernel and the tests use (the same names as the
// RP2350 Cortex-M port, so its SMP tests port unchanged).

extern "C"
{
  // The kernel's CPU index: this hart.
  unsigned
  port_cpu_id (void)
  {
    return riscv::this_hart ();
  }

  // Ask another hart to reschedule: its machine software interrupt.
  void
  port_smp_ipi (unsigned cpu)
  {
    if (cpu >= OS_NCPU || cpu == riscv::this_hart ())
      {
        return;
      }
    clint::msip_set (cpu);
  }

  // The stack a secondary hart starts on (multicore::launch_core1()).
  std::uintptr_t
  port_core1_stack_top (void)
  {
    return reinterpret_cast<std::uintptr_t> (
        &secondary_boot_stack[0][secondary_boot_stack_words]);
  }

  // Release secondary hart `hart`, parked in startup.S: write its entry
  // and stack, then the magic (the hart reads the magic first). For boards
  // where every hart enters at _start (QEMU virt); a board whose ROM holds
  // the other cores (the RP2350) launches them its own way.
  extern std::uintptr_t riscv_secondary_boot[];
  void
  riscv_smp_release_hart (unsigned hart, void (*entry) (void),
                          std::uintptr_t stack_top)
  {
    if (hart == 0 || hart >= OS_NCPU)
      {
        return;
      }
    std::uintptr_t* slot = &riscv_secondary_boot[3 * hart];
    slot[2] = stack_top & ~static_cast<std::uintptr_t> (15);
    slot[1] = reinterpret_cast<std::uintptr_t> (entry);
    __atomic_store_n (&slot[0], static_cast<std::uintptr_t> (RISCV_SECONDARY_MAGIC),
                      __ATOMIC_RELEASE);
  }

  // Nothing to do on RISC-V: the IPI is the software interrupt, enabled for
  // the whole run (on the RP2350 Cortex-M port the FIFO IRQ had to wait for
  // the core-1 launch handshake, which used the same FIFO).
  void
  port_smp_core0_ipi_enable (void)
  {
  }

  // A secondary hart's entry, on its boot stack, gp and mtvec already set
  // (startup.S riscv_secondary_entry), interrupts masked. Like the boot
  // hart's scheduler::start(): a fake current thread, this hart's tick, the
  // software interrupt, then the first switch -- into the thread this hart
  // may run (its idle thread, "idle1", registered before the launch).
  void
  port_smp_secondary_start (void)
  {
    using namespace os::rtos;
    riscv::csr::mstatus_clear_mie ();
    const unsigned h = riscv::this_hart ();

    static os_thread_t fake_thread_sec[OS_NCPU];
    std::memset (&fake_thread_sec[h], 0, sizeof (os_thread_t));
    fake_thread_sec[h].name = "fake_thread_sec";
    scheduler::current_thread_[h]
        = reinterpret_cast<thread*> (&fake_thread_sec[h]);

    clint::msip_clear (h);
    RISCV_CSR_SET (mie, riscv::csr::MIE_MSIE);
    tick_start_this_hart ();

    riscv_port_yield ();

    for (;;)
      {
        __asm__ volatile("" ::: "memory");
      }
  }
}
#endif
