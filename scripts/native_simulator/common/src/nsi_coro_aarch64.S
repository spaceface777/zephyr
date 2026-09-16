/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AArch64 coroutine switch, for the AAPCS64 procedure call standard.
 *
 * Written from the AAPCS64 specification. A switch saves exactly what AAPCS64
 * requires a function to preserve across a call:
 *
 *   x19-x28   callee-saved general purpose registers
 *   x29       frame pointer
 *   x30       link register (the return address)
 *   d8-d15    callee-saved halves of the SIMD/FP registers
 *
 * Caller-saved registers, the full SIMD registers, and the FPCR/FPSR are not
 * saved, because a function call does not preserve them either. The floating
 * point environment is restored in C by the layer above.
 *
 * x18 is never read or written. Apple platforms reserve it, and on others it
 * may be a platform register.
 *
 * Frame layout, 160 bytes, 16 byte aligned:
 *
 *   0x00 d8  d9    0x40 x19 x20    0x80 x27 x28
 *   0x10 d10 d11   0x50 x21 x22    0x90 x29 x30
 *   0x20 d12 d13   0x60 x23 x24
 *   0x30 d14 d15   0x70 x25 x26
 */

#define NSI_CORO_FRAME 0xa0

#if defined(__APPLE__)
#define SYM(name) _##name
#else
#define SYM(name) name
#endif

#if defined(__APPLE__)
.section __TEXT,__text,regular,pure_instructions
#else
.text
#endif

/*
 * nsi_coro_t nsi_coro_create(void *stack_top, size_t stack_size,
 *                            nsi_coro_entry_f entry)
 *
 * x0 stack_top, x1 stack_size, x2 entry.
 */
.balign 16
.globl SYM(nsi_coro_create)
#if !defined(__APPLE__)
.type SYM(nsi_coro_create), %function
#endif
SYM(nsi_coro_create):
	/* Refuse a stack which cannot hold the initial frame. */
	cmp	x1, #NSI_CORO_FRAME
	b.lo	1f

	/* Align the top down to 16 bytes, then reserve the frame below it. */
	and	x0, x0, #~15
	sub	x0, x0, #NSI_CORO_FRAME

	/* Address of the trampoline, which the first restore returns to. */
#if defined(__APPLE__)
	adrp	x3, SYM(nsi_coro_trampoline)@PAGE
	add	x3, x3, SYM(nsi_coro_trampoline)@PAGEOFF
#else
	adrp	x3, SYM(nsi_coro_trampoline)
	add	x3, x3, :lo12:SYM(nsi_coro_trampoline)
#endif

	/*
	 * The whole frame is written, so that the first restore does not load
	 * uninitialized stack into callee-saved registers. Only two slots carry
	 * anything: x19 holds the entry function, because it survives the
	 * restore, and x30 sends that restore to the trampoline. x29 is zero so
	 * an unwinder stops here instead of walking off the end of the stack.
	 */
	stp	xzr, xzr, [x0, #0x00]
	stp	xzr, xzr, [x0, #0x10]
	stp	xzr, xzr, [x0, #0x20]
	stp	xzr, xzr, [x0, #0x30]
	stp	x2,  xzr, [x0, #0x40]		/* x19 = entry, x20 = 0 */
	stp	xzr, xzr, [x0, #0x50]
	stp	xzr, xzr, [x0, #0x60]
	stp	xzr, xzr, [x0, #0x70]
	stp	xzr, xzr, [x0, #0x80]
	stp	xzr, x3,  [x0, #0x90]		/* x29 = 0, x30 = trampoline */
	ret
1:
	mov	x0, xzr
	ret
#if !defined(__APPLE__)
.size SYM(nsi_coro_create), . - SYM(nsi_coro_create)
#endif

/*
 * struct nsi_coro_transfer nsi_coro_switch(nsi_coro_t target, void *argument)
 *
 * x0 target, x1 argument. Returns x0 = from, x1 = argument, which is how
 * AAPCS64 returns a two pointer structure.
 */
.balign 16
.globl SYM(nsi_coro_switch)
#if !defined(__APPLE__)
.type SYM(nsi_coro_switch), %function
#endif
SYM(nsi_coro_switch):
	stp	d8,  d9,  [sp, #-NSI_CORO_FRAME]!
	stp	d10, d11, [sp, #0x10]
	stp	d12, d13, [sp, #0x20]
	stp	d14, d15, [sp, #0x30]
	stp	x19, x20, [sp, #0x40]
	stp	x21, x22, [sp, #0x50]
	stp	x23, x24, [sp, #0x60]
	stp	x25, x26, [sp, #0x70]
	stp	x27, x28, [sp, #0x80]
	stp	x29, x30, [sp, #0x90]

	mov	x2, sp				/* our continuation */
	mov	sp, x0				/* onto the target stack */
	mov	x0, x2				/* transfer.from */
						/* x1 is already transfer.argument */

	ldp	d8,  d9,  [sp, #0x00]
	ldp	d10, d11, [sp, #0x10]
	ldp	d12, d13, [sp, #0x20]
	ldp	d14, d15, [sp, #0x30]
	ldp	x19, x20, [sp, #0x40]
	ldp	x21, x22, [sp, #0x50]
	ldp	x23, x24, [sp, #0x60]
	ldp	x25, x26, [sp, #0x70]
	ldp	x27, x28, [sp, #0x80]
	ldp	x29, x30, [sp, #0x90]
	add	sp, sp, #NSI_CORO_FRAME
	ret
#if !defined(__APPLE__)
.size SYM(nsi_coro_switch), . - SYM(nsi_coro_switch)
#endif

/*
 * First entry of a new coroutine, reached by the ret of nsi_coro_switch.
 *
 * x0 and x1 already hold the transfer, which is how AAPCS64 passes a two
 * pointer structure by value. x19 holds the entry function.
 */
.balign 16
#if !defined(__APPLE__)
.type SYM(nsi_coro_trampoline), %function
#endif
SYM(nsi_coro_trampoline):
	blr	x19
	bl	SYM(nsi_coro_entry_returned)
	brk	#0
#if !defined(__APPLE__)
.size SYM(nsi_coro_trampoline), . - SYM(nsi_coro_trampoline)
.section .note.GNU-stack,"",%progbits
#endif
