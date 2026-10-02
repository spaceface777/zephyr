/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Stackful coroutine switch using the historical POSIX ucontext API.
 *
 * This implements the same small interface as nsi_coro.S so the NCE/NCT
 * coroutine implementation can be compared without changing its scheduling,
 * stack allocation or per-context state handling.
 *
 * swapcontext() also saves and restores the signal mask as part of ucontext_t.
 */

#include <stdlib.h>
#include <ucontext.h>
#include "nsi_coro.h"
#include "nsi_tracing.h"

#ifndef NSI_CORO_STACK_SIZE
#define NSI_CORO_STACK_SIZE (8 * 1024 * 1024)
#endif

struct nsi_coro_uc {
	ucontext_t uc;
	nsi_coro_entry_f entry;
	struct nsi_coro_uc *incoming;
};

static struct nsi_coro_uc host;
static struct nsi_coro_uc *active;

static void nsi_coro_uc_trampoline(void)
{
	struct nsi_coro_uc *self = active;

	self->entry(self->incoming);
	nsi_print_error_and_exit("%s: coroutine entry returned\n", __func__);
}

nsi_coro_t nsi_coro_create(void *stack_top, nsi_coro_entry_f entry)
{
	struct nsi_coro_uc *c = calloc(1, sizeof(*c));

	if (c == NULL) {
		nsi_print_error_and_exit("%s: out of memory\n", __func__);
	}
	if (getcontext(&c->uc) != 0) {
		nsi_print_error_and_exit("%s: getcontext() failed\n", __func__);
	}

	c->entry = entry;
	c->uc.uc_stack.ss_sp = (char *)stack_top - NSI_CORO_STACK_SIZE;
	c->uc.uc_stack.ss_size = NSI_CORO_STACK_SIZE;
	c->uc.uc_stack.ss_flags = 0;
	c->uc.uc_link = NULL;
	makecontext(&c->uc, nsi_coro_uc_trampoline, 0);

	return c;
}

nsi_coro_t nsi_coro_switch(nsi_coro_t target_arg)
{
	struct nsi_coro_uc *target = target_arg;
	struct nsi_coro_uc *from;

	if (active == NULL) {
		active = &host;
	}

	from = active;
	target->incoming = from;
	active = target;

	if (swapcontext(&from->uc, &target->uc) != 0) {
		nsi_print_error_and_exit("%s: swapcontext() failed\n", __func__);
	}

	active = from;
	return from->incoming;
}

void nsi_coro_destroy(nsi_coro_t coro)
{
	free(coro);
}
