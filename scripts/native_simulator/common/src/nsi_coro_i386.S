/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * i386 coroutine switch, for the System V Intel386 ABI.
 *
 * Written from the System V Intel386 ABI specification. A switch saves exactly
 * what the ABI requires a function to preserve across a call:
 *
 *   ebx, esi, edi, ebp   callee-saved registers
 *   the return address   already on the stack at entry
 *
 * The x87 and SSE state is not saved. This ABI has no callee-saved data
 * register there, and the floating point environment, which does include the
 * control words, is saved and restored in C by the layer above.
 *
 * A switch returns a single pointer, which this ABI returns in eax, so this
 * port is the same shape as the 64 bit ones. Returning two pointers would have
 * been returned in memory here, through a hidden pointer argument popped by the
 * callee with `ret $4`, which is why the primitive returns one.
 *
 * This file is built only for ELF. The macOS port is arm64 only.
 *
 * Note on control flow integrity: this switches stacks with a plain ret, so it
 * is incompatible with a shadow stack (CET SHSTK).
 *
 * Frame layout, 20 bytes, lowest address first:
 *
 *   0x00 edi   0x04 esi   0x08 ebx   0x0c ebp   0x10 return address
 */

	.text

/*
 * nsi_coro_t nsi_coro_create(void *stack_top, size_t stack_size,
 *                            nsi_coro_entry_f entry)
 *
 * 4(%esp) stack_top, 8(%esp) stack_size, 12(%esp) entry. Returns in eax.
 */
	.balign 16
	.globl nsi_coro_create
	.type nsi_coro_create, @function
nsi_coro_create:
	endbr32
	/* Refuse a stack which cannot hold the initial frame. */
	cmpl	$64, 8(%esp)
	jb	2f

	movl	4(%esp), %eax			/* stack_top */
	movl	12(%esp), %edx			/* entry */

	/*
	 * Align the top down, then reserve the frame. The restore pops 16 bytes
	 * and the ret pops 4, so the trampoline starts on the aligned top, which
	 * is the alignment this ABI requires at a call.
	 */
	andl	$-16, %eax
	subl	$20, %eax

	movl	$0, 0x00(%eax)			/* edi */
	movl	$0, 0x04(%eax)			/* esi */
	movl	%edx, 0x08(%eax)		/* ebx = entry, survives the restore */
	movl	$0, 0x0c(%eax)			/* ebp = 0, terminates unwinding */

	/*
	 * The address of the trampoline, as the distance between two labels in
	 * this section. That is a link time constant, so this needs no
	 * relocation against a text address and works in a position independent
	 * build as well.
	 */
	call	1f
1:
	popl	%edx
	addl	$nsi_coro_trampoline-1b, %edx
	movl	%edx, 0x10(%eax)		/* return address */
	ret
2:
	xorl	%eax, %eax
	ret
	.size nsi_coro_create, . - nsi_coro_create

/*
 * nsi_coro_t nsi_coro_switch(nsi_coro_t target)
 *
 * 4(%esp) target. Returns eax = the continuation of whoever switches back.
 */
	.balign 16
	.globl nsi_coro_switch
	.type nsi_coro_switch, @function
nsi_coro_switch:
	endbr32
	movl	4(%esp), %ecx			/* target */

	pushl	%ebp
	pushl	%ebx
	pushl	%esi
	pushl	%edi

	movl	%esp, %eax			/* our continuation, and the result */
	movl	%ecx, %esp			/* onto the target stack */

	popl	%edi
	popl	%esi
	popl	%ebx
	popl	%ebp
	ret
	.size nsi_coro_switch, . - nsi_coro_switch

/*
 * First entry of a new coroutine, reached by the ret of nsi_coro_switch.
 *
 * eax holds the continuation of whoever switched here, and ebx holds the entry
 * function, which takes that one pointer on the stack.
 */
	.balign 16
	.type nsi_coro_trampoline, @function
nsi_coro_trampoline:
	endbr32
	subl	$12, %esp			/* keep esp 16 byte aligned at the call */
	pushl	%eax
	call	*%ebx
	call	nsi_coro_entry_returned
	ud2
	.size nsi_coro_trampoline, . - nsi_coro_trampoline

	.section .note.GNU-stack,"",@progbits
