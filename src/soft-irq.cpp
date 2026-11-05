/*
 * soft-irq.cpp -- software interrupt lines on the machine software interrupt.
 *
 * A RISC-V hart has no NVIC, and its one software-triggered interrupt (CLINT
 * MSIP) is already the port's context-switch request. Lines 0..31 are
 * multiplexed onto it: NVIC_SetPendingIRQ(n) marks line n and raises MSIP;
 * the MSIP trap (trap.cpp) first runs every pending, enabled line's handler
 * IN HANDLER MODE -- interrupts masked, riscv_trap_nesting != 0, so the RTOS
 * ISR-safe calls behave as they do in any interrupt -- and only then
 * switches threads, if a reschedule is pending.
 *
 * The NVIC names are what Cortex-M code calls (the CMSIS-RTOS validator
 * raises "IRQ 0" this way, cmsis_device.h declares them). A user of a line
 * defines riscv_soft_irq_handler(); the validator's board glue does
 * (test/milkv-duo/cmsis-os-validator/harness-suite.cpp).
 *
 * Copyright (c) 2026 Dan. MIT license.
 */

#include <cstdint>

#include <clint.hpp>
#include <cmsis_device.h>

extern "C"
{
  volatile std::uint32_t riscv_soft_irq_enabled = 0;
  volatile std::uint32_t riscv_soft_irq_pending = 0;

  void __attribute__ ((weak)) riscv_soft_irq_handler (unsigned line)
  {
    (void) line;
  }

  void riscv_soft_irq_dispatch (void);
}

namespace
{
  inline std::uint32_t
  bit (IRQn_Type n)
  {
    return (n >= 0 && n < 32) ? (1u << static_cast<unsigned> (n)) : 0u;
  }
} // namespace

// Enabling a line DISCARDS a request left pending while it was disabled,
// the semantics the Raspberry Pi ports give their Mailbox-1 stand-in for the
// same validator. It matters: TC_MailFromThreadToISR enables the line before
// it empties the mail queue, and the previous case's periodic timer may have
// raised the line after that case disabled it. Delivering the stale request
// at enable ran the new case's ISR on the old case's mail.
void
NVIC_EnableIRQ (IRQn_Type n)
{
  __atomic_fetch_and (&riscv_soft_irq_pending, ~bit (n), __ATOMIC_SEQ_CST);
  __atomic_fetch_or (&riscv_soft_irq_enabled, bit (n), __ATOMIC_SEQ_CST);
}

void
NVIC_DisableIRQ (IRQn_Type n)
{
  __atomic_fetch_and (&riscv_soft_irq_enabled, ~bit (n), __ATOMIC_SEQ_CST);
}

void
NVIC_SetPendingIRQ (IRQn_Type n)
{
  __atomic_fetch_or (&riscv_soft_irq_pending, bit (n), __ATOMIC_SEQ_CST);
  if (riscv_soft_irq_enabled & bit (n))
    {
      clint::msip_set ();
    }
}

void
NVIC_ClearPendingIRQ (IRQn_Type n)
{
  __atomic_fetch_and (&riscv_soft_irq_pending, ~bit (n), __ATOMIC_SEQ_CST);
}

// Called from the MSIP trap, interrupts masked.
void
riscv_soft_irq_dispatch (void)
{
  for (;;)
    {
      const std::uint32_t ready = riscv_soft_irq_pending & riscv_soft_irq_enabled;
      if (ready == 0)
        {
          return;
        }
      const unsigned line = static_cast<unsigned> (__builtin_ctz (ready));
      __atomic_fetch_and (&riscv_soft_irq_pending, ~(1u << line),
                          __ATOMIC_SEQ_CST);
      riscv_soft_irq_handler (line);
    }
}
