/*
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 */

/*
 * os-inlines.h -- the inline half of the RISC-V port (RV64, M-mode).
 *
 * Critical sections are mstatus.MIE: `csrrci` clears it and returns the old
 * value in one instruction, so enter() is atomic with respect to interrupts
 * without a read-modify-write window. "In handler mode" is the trap nesting
 * count kept by the trap entry, because RISC-V has no IPSR equivalent.
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_
#define CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_

#if defined(OS_USE_OS_APP_CONFIG_H)
#include <cmsis-plus/os-app-config.h>
#endif

#include <cmsis-plus/rtos/os-c-decls.h>

#ifdef __cplusplus

#include <cmsis-plus/diag/trace.h>
#include <riscv-csr.hpp>
#include <riscv-board.hpp>

extern "C"
{
  // Trap nesting depth, maintained by riscv_trap_entry (src/trap.S).
  extern volatile uint32_t riscv_trap_nesting;
  // rdtime value at the last system tick (src/rtos/os-core.cpp).
  extern volatile uint64_t riscv_port_last_tick;
}

namespace os
{
  namespace rtos
  {
    namespace port
    {
      namespace scheduler
      {
        inline void __attribute__ ((always_inline))
        greeting (void)
        {
          // PORT_GREETING names the board; board.cmake defines it.
          trace::printf ("Scheduler: " PORT_GREETING
                         ", RV64 M-mode, preemptive, %u Hz tick, MSIP switch\n",
                         static_cast<unsigned> (OS_INTEGER_SYSTICK_FREQUENCY_HZ));
          trace::printf ("Timebase: %u Hz\n",
                         static_cast<unsigned> (riscv_board::TIMEBASE_HZ));
        }

        inline port::scheduler::state_t __attribute__ ((always_inline))
        lock (void)
        {
          return locked (state::locked);
        }

        inline port::scheduler::state_t __attribute__ ((always_inline))
        unlock (void)
        {
          return locked (state::unlocked);
        }

        inline bool __attribute__ ((always_inline))
        locked (void)
        {
          return lock_state != state::unlocked;
        }

        inline void __attribute__ ((always_inline))
        wait_for_interrupt (void)
        {
#if !defined(OS_EXCLUDE_RTOS_IDLE_SLEEP)
          __asm__ volatile("fence iorw, iorw\n\twfi" ::: "memory");
#endif
        }
      } /* namespace scheduler */

      namespace interrupts
      {
        inline bool __attribute__ ((always_inline))
        in_handler_mode (void)
        {
          return riscv_trap_nesting != 0;
        }

        inline bool
        is_priority_valid (void)
        {
          return true;
        }

        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        critical_section::enter (void)
        {
          return riscv::csr::mstatus_clear_mie ();
        }

        inline void __attribute__ ((always_inline))
        critical_section::exit (rtos::interrupts::state_t state)
        {
          if (state & riscv::csr::MSTATUS_MIE)
            {
              riscv::csr::mstatus_set_mie ();
            }
        }

        inline rtos::interrupts::state_t __attribute__ ((always_inline))
        uncritical_section::enter (void)
        {
          return riscv::csr::mstatus_set_mie ();
        }

        inline void __attribute__ ((always_inline))
        uncritical_section::exit (rtos::interrupts::state_t state)
        {
          if (!(state & riscv::csr::MSTATUS_MIE))
            {
              riscv::csr::mstatus_clear_mie ();
            }
        }
      } /* namespace interrupts */

      namespace this_thread
      {
        inline void __attribute__ ((always_inline))
        prepare_suspend (void)
        {
        }
      } /* namespace this_thread */

      inline void __attribute__ ((always_inline))
      clock_highres::start (void)
      {
      }

      // The high resolution clock is derived from the tick count plus the
      // cycles since the last tick, the way the upstream Cortex-M port does
      // it; the cycles are rdtime ticks.
      inline constexpr bool __attribute__ ((always_inline))
      clock_highres::has_hardware_counter (void) noexcept
      {
        return false;
      }

      inline uint64_t __attribute__ ((always_inline))
      clock_highres::hardware_counter (void) noexcept
      {
        return 0;
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::input_clock_frequency_hz (void)
      {
        return riscv_board::TIMEBASE_HZ;
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::cycles_per_tick (void)
      {
        return riscv_board::TIMEBASE_HZ / OS_INTEGER_SYSTICK_FREQUENCY_HZ;
      }

      inline uint32_t __attribute__ ((always_inline))
      clock_highres::cycles_since_tick (void)
      {
        const uint64_t now = riscv::csr::rdtime ();
        const uint64_t since = now - riscv_port_last_tick;
        const uint32_t per = cycles_per_tick ();
        return (since >= per) ? (per - 1) : static_cast<uint32_t> (since);
      }

    } /* namespace port */
  } /* namespace rtos */
} /* namespace os */

#endif /* __cplusplus */

#endif /* CMSIS_PLUS_RTOS_PORT_OS_INLINES_H_ */
