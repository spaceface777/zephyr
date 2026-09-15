/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NCT_CONTEXT_H
#define NCT_CONTEXT_H

#include <stddef.h>
#include <stdint.h>
#include "nct_context_offsets.h"

struct nct_context {
	uintptr_t rsp;
	uintptr_t rip;
	uintptr_t rbx;
	uintptr_t rbp;
	uintptr_t r12;
	uintptr_t r13;
	uintptr_t r14;
	uintptr_t r15;
	uint32_t mxcsr;
	uint16_t x87_cw;
	uint16_t reserved;
};

_Static_assert(offsetof(struct nct_context, rsp) == NCT_CTX_RSP, "rsp offset mismatch");
_Static_assert(offsetof(struct nct_context, rip) == NCT_CTX_RIP, "rip offset mismatch");
_Static_assert(offsetof(struct nct_context, rbx) == NCT_CTX_RBX, "rbx offset mismatch");
_Static_assert(offsetof(struct nct_context, rbp) == NCT_CTX_RBP, "rbp offset mismatch");
_Static_assert(offsetof(struct nct_context, r12) == NCT_CTX_R12, "r12 offset mismatch");
_Static_assert(offsetof(struct nct_context, r13) == NCT_CTX_R13, "r13 offset mismatch");
_Static_assert(offsetof(struct nct_context, r14) == NCT_CTX_R14, "r14 offset mismatch");
_Static_assert(offsetof(struct nct_context, r15) == NCT_CTX_R15, "r15 offset mismatch");
_Static_assert(offsetof(struct nct_context, mxcsr) == NCT_CTX_MXCSR, "mxcsr offset mismatch");
_Static_assert(offsetof(struct nct_context, x87_cw) == NCT_CTX_X87_CW, "x87 offset mismatch");

void nct_context_switch(struct nct_context *from, const struct nct_context *to);
void nct_context_bootstrap(void);

#endif /* NCT_CONTEXT_H */
