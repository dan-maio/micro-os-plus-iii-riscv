/*
 * cmsis_device.h -- the kernel includes <cmsis_device.h> from code compiled
 * under __ARM_EABI__ (src/libc/stdlib/exit.c). A RISC-V hart has no CMSIS
 * core; this stub gives the few names that code reaches, as the AArch32 and
 * AArch64 ports' stubs do.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */
#ifndef CMSIS_DEVICE_H_
#define CMSIS_DEVICE_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

static inline void
NVIC_SystemReset (void)
{
  for (;;)
    {
      __asm__ volatile("wfi");
    }
}

#define __NOP() __asm__ volatile("nop")
#define __BKPT(n) __asm__ volatile("ebreak")

extern uint32_t SystemCoreClock;
static inline void
SystemCoreClockUpdate (void)
{
}

#ifdef __cplusplus
}
#endif

#endif /* CMSIS_DEVICE_H_ */
