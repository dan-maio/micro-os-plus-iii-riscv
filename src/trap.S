/*
 * trap.S -- the RISC-V port's single trap entry (machine mode, direct mtvec).
 *
 * Every interrupt and exception comes here. The frame (port_ctx.hpp) is
 * built on the stack that was interrupted -- the running thread's -- and
 * the C dispatcher runs on the interrupt stack. The dispatcher returns the
 * frame to resume: the same one, or the incoming thread's (a thread's yield
 * ecall, or a trap that made another thread due). Restoring from a
 * different frame IS the context switch.
 *
 * Interrupts stay masked for the whole trap (mstatus.MIE is cleared by the
 * hardware on entry and comes back from MPIE on mret), so interrupts never
 * nest; only an exception inside a handler can (nesting 2), and such a trap
 * never switches. riscv_trap_nesting is what in_handler_mode() reads.
 *
 * Copyright (c) 2026 Dan. MIT license.
 */

#include <port_ctx.hpp>

	.section .text.trap, "ax"
	.balign 64
	.globl	riscv_trap_entry
	.type	riscv_trap_entry, @function
riscv_trap_entry:
	addi	sp, sp, -PORT_CTX_SIZE

	sd	x1,  PORT_CTX_X(1)(sp)
	sd	x3,  PORT_CTX_X(3)(sp)
	sd	x4,  PORT_CTX_X(4)(sp)
	sd	x5,  PORT_CTX_X(5)(sp)
	sd	x6,  PORT_CTX_X(6)(sp)
	sd	x7,  PORT_CTX_X(7)(sp)
	sd	x8,  PORT_CTX_X(8)(sp)
	sd	x9,  PORT_CTX_X(9)(sp)
	sd	x10, PORT_CTX_X(10)(sp)
	sd	x11, PORT_CTX_X(11)(sp)
	sd	x12, PORT_CTX_X(12)(sp)
	sd	x13, PORT_CTX_X(13)(sp)
	sd	x14, PORT_CTX_X(14)(sp)
	sd	x15, PORT_CTX_X(15)(sp)
	sd	x16, PORT_CTX_X(16)(sp)
	sd	x17, PORT_CTX_X(17)(sp)
	sd	x18, PORT_CTX_X(18)(sp)
	sd	x19, PORT_CTX_X(19)(sp)
	sd	x20, PORT_CTX_X(20)(sp)
	sd	x21, PORT_CTX_X(21)(sp)
	sd	x22, PORT_CTX_X(22)(sp)
	sd	x23, PORT_CTX_X(23)(sp)
	sd	x24, PORT_CTX_X(24)(sp)
	sd	x25, PORT_CTX_X(25)(sp)
	sd	x26, PORT_CTX_X(26)(sp)
	sd	x27, PORT_CTX_X(27)(sp)
	sd	x28, PORT_CTX_X(28)(sp)
	sd	x29, PORT_CTX_X(29)(sp)
	sd	x30, PORT_CTX_X(30)(sp)
	sd	x31, PORT_CTX_X(31)(sp)

	csrr	t0, mepc
	sd	t0, PORT_CTX_MEPC(sp)
	csrr	t0, mstatus
	sd	t0, PORT_CTX_MSTATUS(sp)

	/* The FPU is always on (mstatus.FS != Off for every thread), so the FP
	 * file is saved unconditionally: a preemptive switch can land in the
	 * middle of any floating-point sequence. */
	frcsr	t0
	sd	t0, PORT_CTX_FCSR(sp)
	fsd	f0,  PORT_CTX_F(0)(sp)
	fsd	f1,  PORT_CTX_F(1)(sp)
	fsd	f2,  PORT_CTX_F(2)(sp)
	fsd	f3,  PORT_CTX_F(3)(sp)
	fsd	f4,  PORT_CTX_F(4)(sp)
	fsd	f5,  PORT_CTX_F(5)(sp)
	fsd	f6,  PORT_CTX_F(6)(sp)
	fsd	f7,  PORT_CTX_F(7)(sp)
	fsd	f8,  PORT_CTX_F(8)(sp)
	fsd	f9,  PORT_CTX_F(9)(sp)
	fsd	f10, PORT_CTX_F(10)(sp)
	fsd	f11, PORT_CTX_F(11)(sp)
	fsd	f12, PORT_CTX_F(12)(sp)
	fsd	f13, PORT_CTX_F(13)(sp)
	fsd	f14, PORT_CTX_F(14)(sp)
	fsd	f15, PORT_CTX_F(15)(sp)
	fsd	f16, PORT_CTX_F(16)(sp)
	fsd	f17, PORT_CTX_F(17)(sp)
	fsd	f18, PORT_CTX_F(18)(sp)
	fsd	f19, PORT_CTX_F(19)(sp)
	fsd	f20, PORT_CTX_F(20)(sp)
	fsd	f21, PORT_CTX_F(21)(sp)
	fsd	f22, PORT_CTX_F(22)(sp)
	fsd	f23, PORT_CTX_F(23)(sp)
	fsd	f24, PORT_CTX_F(24)(sp)
	fsd	f25, PORT_CTX_F(25)(sp)
	fsd	f26, PORT_CTX_F(26)(sp)
	fsd	f27, PORT_CTX_F(27)(sp)
	fsd	f28, PORT_CTX_F(28)(sp)
	fsd	f29, PORT_CTX_F(29)(sp)
	fsd	f30, PORT_CTX_F(30)(sp)
	fsd	f31, PORT_CTX_F(31)(sp)

	/* gp may have been changed by nothing (we never write it), but reload it
	 * so the dispatcher's gp-relative accesses cannot depend on that. */
	.option push
	.option norelax
	la	gp, __global_pointer$
	.option pop

	/* a0 = the frame. Count the nesting and move to the interrupt stack. */
	mv	a0, sp
	la	t0, riscv_trap_nesting
	lw	t1, 0(t0)
	addiw	t2, t1, 1
	sw	t2, 0(t0)
	bnez	t1, 1f
	la	sp, __irq_stack_top
1:
	call	riscv_trap_dispatch

	/* a0 = the frame to resume (another thread's after a switch). */
	la	t0, riscv_trap_nesting
	lw	t1, 0(t0)
	addiw	t1, t1, -1
	sw	t1, 0(t0)
	mv	sp, a0

	.globl	riscv_trap_restore
riscv_trap_restore:
	ld	t0, PORT_CTX_MSTATUS(sp)
	csrw	mstatus, t0
	ld	t0, PORT_CTX_MEPC(sp)
	csrw	mepc, t0

	ld	t0, PORT_CTX_FCSR(sp)
	fscsr	t0
	fld	f0,  PORT_CTX_F(0)(sp)
	fld	f1,  PORT_CTX_F(1)(sp)
	fld	f2,  PORT_CTX_F(2)(sp)
	fld	f3,  PORT_CTX_F(3)(sp)
	fld	f4,  PORT_CTX_F(4)(sp)
	fld	f5,  PORT_CTX_F(5)(sp)
	fld	f6,  PORT_CTX_F(6)(sp)
	fld	f7,  PORT_CTX_F(7)(sp)
	fld	f8,  PORT_CTX_F(8)(sp)
	fld	f9,  PORT_CTX_F(9)(sp)
	fld	f10, PORT_CTX_F(10)(sp)
	fld	f11, PORT_CTX_F(11)(sp)
	fld	f12, PORT_CTX_F(12)(sp)
	fld	f13, PORT_CTX_F(13)(sp)
	fld	f14, PORT_CTX_F(14)(sp)
	fld	f15, PORT_CTX_F(15)(sp)
	fld	f16, PORT_CTX_F(16)(sp)
	fld	f17, PORT_CTX_F(17)(sp)
	fld	f18, PORT_CTX_F(18)(sp)
	fld	f19, PORT_CTX_F(19)(sp)
	fld	f20, PORT_CTX_F(20)(sp)
	fld	f21, PORT_CTX_F(21)(sp)
	fld	f22, PORT_CTX_F(22)(sp)
	fld	f23, PORT_CTX_F(23)(sp)
	fld	f24, PORT_CTX_F(24)(sp)
	fld	f25, PORT_CTX_F(25)(sp)
	fld	f26, PORT_CTX_F(26)(sp)
	fld	f27, PORT_CTX_F(27)(sp)
	fld	f28, PORT_CTX_F(28)(sp)
	fld	f29, PORT_CTX_F(29)(sp)
	fld	f30, PORT_CTX_F(30)(sp)
	fld	f31, PORT_CTX_F(31)(sp)

	ld	x1,  PORT_CTX_X(1)(sp)
	ld	x3,  PORT_CTX_X(3)(sp)
	ld	x4,  PORT_CTX_X(4)(sp)
	ld	x5,  PORT_CTX_X(5)(sp)
	ld	x6,  PORT_CTX_X(6)(sp)
	ld	x7,  PORT_CTX_X(7)(sp)
	ld	x8,  PORT_CTX_X(8)(sp)
	ld	x9,  PORT_CTX_X(9)(sp)
	ld	x10, PORT_CTX_X(10)(sp)
	ld	x11, PORT_CTX_X(11)(sp)
	ld	x12, PORT_CTX_X(12)(sp)
	ld	x13, PORT_CTX_X(13)(sp)
	ld	x14, PORT_CTX_X(14)(sp)
	ld	x15, PORT_CTX_X(15)(sp)
	ld	x16, PORT_CTX_X(16)(sp)
	ld	x17, PORT_CTX_X(17)(sp)
	ld	x18, PORT_CTX_X(18)(sp)
	ld	x19, PORT_CTX_X(19)(sp)
	ld	x20, PORT_CTX_X(20)(sp)
	ld	x21, PORT_CTX_X(21)(sp)
	ld	x22, PORT_CTX_X(22)(sp)
	ld	x23, PORT_CTX_X(23)(sp)
	ld	x24, PORT_CTX_X(24)(sp)
	ld	x25, PORT_CTX_X(25)(sp)
	ld	x26, PORT_CTX_X(26)(sp)
	ld	x27, PORT_CTX_X(27)(sp)
	ld	x28, PORT_CTX_X(28)(sp)
	ld	x29, PORT_CTX_X(29)(sp)
	ld	x30, PORT_CTX_X(30)(sp)
	ld	x31, PORT_CTX_X(31)(sp)

	addi	sp, sp, PORT_CTX_SIZE
	mret
	.size	riscv_trap_entry, . - riscv_trap_entry

	.section .bss
	.balign 4
	.globl	riscv_trap_nesting
riscv_trap_nesting:
	.word	0
