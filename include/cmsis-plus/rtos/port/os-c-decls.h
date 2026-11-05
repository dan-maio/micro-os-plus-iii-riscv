/*
 * This file is part of the µOS++ project (https://micro-os-plus.github.io/).
 * Copyright (c) Liviu Ionescu. All rights reserved.
 * Copyright (c) 2026 Dan. All rights reserved.
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose is hereby granted, under the terms of the MIT license.
 */

/*
 * os-c-decls.h -- the C half of the RISC-V port contract (RV64, M-mode).
 *
 * Everything width-dependent lives here: RV64 has 64-bit registers, so the
 * stack element, the saved stack pointer and the interrupt state (a copy of
 * mstatus.MIE) are 64 bits wide.
 */

#ifndef CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_
#define CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_

#include <stdint.h>
#include <stdbool.h>

#if !defined(__riscv) || (__riscv_xlen != 64)
#error "The micro-os-plus-iii-riscv port is RV64 only (-march=rv64...)."
#endif

typedef uint64_t os_port_clock_timestamp_t;
typedef uint32_t os_port_clock_duration_t;
typedef uint64_t os_port_clock_offset_t;

typedef bool os_port_scheduler_state_t;

/* The saved mstatus.MIE bit (bit 3), 0 or 8. */
typedef uint64_t os_port_irq_state_t;

typedef uint64_t os_port_thread_stack_element_t;
/* The psABI wants 16-byte stack alignment; context::create() aligns the top,
   the allocation element only has to keep the 8-byte register slots aligned. */
typedef uint64_t os_port_thread_stack_allocation_element_t;

typedef struct
{
  os_port_thread_stack_element_t* stack_ptr;
} os_port_thread_context_t;

/* OS_NCPU is a BOARD fact (test/boards/<board>/board.cmake -> NCPU) and has
 * no default here: a board that does not say how many CPUs it has does not
 * compile. The first board, the Milk-V Duo, runs µOS++ on its one main C906
 * hart; the second C906 (the "little" core) is not driven by this port. */
#ifndef OS_NCPU
#error "OS_NCPU is a board fact: set UOS_BOARD_NCPU in test/boards/<board>/board.cmake."
#endif
#if (OS_NCPU != 1)
#error "The RISC-V port is single-hart for now (OS_NCPU=1)."
#endif
#if defined(OS_USE_SMP_SCHEDULER)
#error "The RISC-V port does not implement the SMP scheduler yet."
#endif

#define OS_INTEGER_RTOS_STACK_FILL_MAGIC (0xEFBEADDEEFBEADDEULL)

#define OS_HAS_INTERRUPTS_STACK

#endif /* CMSIS_PLUS_RTOS_PORT_OS_C_DECLS_H_ */
