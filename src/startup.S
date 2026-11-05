/*
 * startup.S -- reset entry of a µOS++ RISC-V image (machine mode, hart 0).
 *
 * On the Milk-V Duo this is NOT the reset vector: the boot ROM, the FSBL and
 * the minimal fip.bin have already brought up the clocks, DDR and UART4, and
 * OpenOCD loads the image into DDR and sets the PC here (test/boards/
 * milkv-duo/hw.sh). Under QEMU (-bios none) the virt machine starts here.
 * Either way the state left by whoever ran before is distrusted: interrupts
 * off, our own trap vector, our own stack, .bss cleared.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */

	.section .text.entry, "ax"
#include <port_ctx.hpp>

	.globl	_start
	.type	_start, @function
_start:
	/* Interrupts off, every source off, nothing pending we caused. */
	csrci	mstatus, 8
	csrw	mie, zero

	/* Our trap vector (direct mode). */
	la	t0, riscv_trap_entry
	csrw	mtvec, t0

	.option push
	.option norelax
	la	gp, __global_pointer$
	.option pop
	mv	tp, zero

	/* The boot stack: main() runs on it until the scheduler starts. */
	la	sp, __stack_top

	/* MPRV off; with an FPU: FPU on (FS = Initial), clean rounding state. */
	li	t0, (1 << 17)
	csrc	mstatus, t0
#if defined(__riscv_flen)
	li	t0, (3 << 13)
	csrc	mstatus, t0
	li	t0, (1 << 13)
	csrs	mstatus, t0
	fscsr	zero
#endif

	/* The image was just written by the debugger: drop stale I-cache lines. */
	fence.i

	/* Zero .bss (a register at a time; the linker aligns both ends). */
	la	t0, __bss_start
	la	t1, __bss_end
1:	bgeu	t0, t1, 2f
	REG_S	zero, 0(t0)
	addi	t0, t0, PORT_REGBYTES
	j	1b
2:
	call	os_startup_initialize_hardware_early
	call	os_startup_initialize_hardware
	call	os_run_init_array

	li	a0, 0
	li	a1, 0
	call	main
	call	exit

3:	j	3b
	.size	_start, . - _start
