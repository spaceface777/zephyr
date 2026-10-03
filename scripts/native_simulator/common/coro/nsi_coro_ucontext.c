/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <ucontext.h>
#include "nsi_coro.h"

struct continuation {
	ucontext_t context;
	struct continuation *from;
	nsi_coro_entry_f entry;
};

static _Thread_local struct continuation *starting;

static void trampoline(void)
{
	struct continuation *self = starting;

	self->entry(self->from);
	abort();
}

nsi_coro_t nsi_coro_create(void *stack_top, nsi_coro_entry_f entry)
{
	struct continuation *continuation =
		(void *)((char *)stack_top - NSI_CORO_STACK_SIZE);
	uintptr_t stack_bottom = ((uintptr_t)(continuation + 1) + 15U) & ~(uintptr_t)15U;

	if (getcontext(&continuation->context) != 0) {
		abort();
	}
	continuation->entry = entry;
	continuation->from = NULL;
	continuation->context.uc_link = NULL;
	continuation->context.uc_stack.ss_sp = (void *)stack_bottom;
	continuation->context.uc_stack.ss_size = (uintptr_t)stack_top - stack_bottom;
	continuation->context.uc_stack.ss_flags = 0;
	makecontext(&continuation->context, trampoline, 0);
	return continuation;
}

nsi_coro_t nsi_coro_switch(nsi_coro_t target)
{
	struct continuation self;
	struct continuation *to = target;

	to->from = &self;
	starting = to;
	if (swapcontext(&self.context, &to->context) != 0) {
		abort();
	}
	starting = NULL;
	return self.from;
}
