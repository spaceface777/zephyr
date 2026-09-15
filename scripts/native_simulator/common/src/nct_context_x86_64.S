/* SPDX-License-Identifier: Apache-2.0 */
#include "nct_context_offsets.h"

	.text
	.globl nct_context_switch
	.type nct_context_switch,@function
nct_context_switch:
	movq %rsp, NCT_CTX_RSP(%rdi)
	leaq .Lresume(%rip), %rax
	movq %rax, NCT_CTX_RIP(%rdi)
	movq %rbx, NCT_CTX_RBX(%rdi)
	movq %rbp, NCT_CTX_RBP(%rdi)
	movq %r12, NCT_CTX_R12(%rdi)
	movq %r13, NCT_CTX_R13(%rdi)
	movq %r14, NCT_CTX_R14(%rdi)
	movq %r15, NCT_CTX_R15(%rdi)
	stmxcsr NCT_CTX_MXCSR(%rdi)
	fnstcw NCT_CTX_X87_CW(%rdi)

	movq NCT_CTX_RSP(%rsi), %rsp
	movq NCT_CTX_RBX(%rsi), %rbx
	movq NCT_CTX_RBP(%rsi), %rbp
	movq NCT_CTX_R12(%rsi), %r12
	movq NCT_CTX_R13(%rsi), %r13
	movq NCT_CTX_R14(%rsi), %r14
	movq NCT_CTX_R15(%rsi), %r15
	ldmxcsr NCT_CTX_MXCSR(%rsi)
	fldcw NCT_CTX_X87_CW(%rsi)
	movq NCT_CTX_RIP(%rsi), %rax
	jmp *%rax
.Lresume:
	ret
	.size nct_context_switch, .-nct_context_switch

	.globl nct_context_bootstrap
	.type nct_context_bootstrap,@function
nct_context_bootstrap:
#ifdef __CET__
#if (__CET__ & 1)
	endbr64
#endif
#endif
	movq %r12, %rdi
	call *%r13
	ud2
	.size nct_context_bootstrap, .-nct_context_bootstrap

	.section .note.GNU-stack,"",@progbits
