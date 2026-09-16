/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * x86-64 coroutine switch, for the System V AMD64 ABI.
 *
 * Written from the System V AMD64 ABI specification. A switch saves exactly
 * what the ABI requires a function to preserve across a call:
 *
 *   rbx, rbp, r12, r13, r14, r15   callee-saved registers
 *   the return address             already on the stack at entry
 *
 * Caller-saved registers and the SSE state are not saved, because a function
 * call does not preserve them either. MXCSR and the x87 control word are not
 * saved here; the floating point environment is restored in C by the layer
 * above, which covers both.
 *
 * Note on control flow integrity: this switches stacks with a plain ret, so it
 * is incompatible with a shadow stack (CET SHSTK). endbr64 is emitted so that
 * indirect branch tracking (CET IBT) is satisfied.
 *
 * Frame layout, 56 bytes at entry to the restore, lowest address first:
 *
 *   0x00 r15   0x08 r14   0x10 r13
 *   0x18 r12   0x20 rbx   0x28 rbp   0x30 return address
 */

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
 * rdi stack_top, rsi stack_size, rdx entry.
 */
.balign 16
.globl SYM(nsi_coro_create)
#if !defined(__APPLE__)
.type SYM(nsi_coro_create), @function
#endif
SYM(nsi_coro_create):
	endbr64
	/* Refuse a stack which cannot hold the initial frame. */
	cmpq	$64, %rsi
	jb	1f

	/*
	 * Align the top down to 16 bytes, then reserve the frame. The return
	 * address slot ends up 8 below a 16 byte boundary, so the trampoline
	 * starts with the alignment an ordinary call would have produced.
	 */
	andq	$-16, %rdi
	subq	$56, %rdi

	xorq	%rax, %rax
	movq	%rax, 0x00(%rdi)		/* r15 */
	movq	%rax, 0x08(%rdi)		/* r14 */
	movq	%rax, 0x10(%rdi)		/* r13 */
	movq	%rax, 0x18(%rdi)		/* r12 */
	movq	%rdx, 0x20(%rdi)		/* rbx = entry, survives restore */
	movq	%rax, 0x28(%rdi)		/* rbp = 0, terminates unwinding */
	leaq	SYM(nsi_coro_trampoline)(%rip), %rax
	movq	%rax, 0x30(%rdi)		/* return address */

	movq	%rdi, %rax
	ret
1:
	xorq	%rax, %rax
	ret
#if !defined(__APPLE__)
.size SYM(nsi_coro_create), . - SYM(nsi_coro_create)
#endif

/*
 * struct nsi_coro_transfer nsi_coro_switch(nsi_coro_t target, void *argument)
 *
 * rdi target, rsi argument. Returns rax = from, rdx = argument, which is how
 * the ABI returns a two pointer structure.
 */
.balign 16
.globl SYM(nsi_coro_switch)
#if !defined(__APPLE__)
.type SYM(nsi_coro_switch), @function
#endif
SYM(nsi_coro_switch):
	endbr64
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	movq	%rsp, %rax			/* our continuation */
	movq	%rdi, %rsp			/* onto the target stack */
	movq	%rsi, %rdx			/* transfer.argument */
						/* rax is already transfer.from */

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret
#if !defined(__APPLE__)
.size SYM(nsi_coro_switch), . - SYM(nsi_coro_switch)
#endif

/*
 * First entry of a new coroutine, reached by the ret of nsi_coro_switch.
 *
 * rax and rdx hold the transfer; rbx holds the entry function. The ABI passes
 * a two pointer structure by value in rdi and rsi.
 */
.balign 16
#if !defined(__APPLE__)
.type SYM(nsi_coro_trampoline), @function
#endif
SYM(nsi_coro_trampoline):
	endbr64
	movq	%rax, %rdi
	movq	%rdx, %rsi
	call	*%rbx
	call	SYM(nsi_coro_entry_returned)
	ud2
#if !defined(__APPLE__)
.size SYM(nsi_coro_trampoline), . - SYM(nsi_coro_trampoline)
.section .note.GNU-stack,"",@progbits
#endif
