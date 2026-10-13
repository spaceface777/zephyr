/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif

#include <windows.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <xmmintrin.h>
#include "nsi_coro_ctx.h"
#include "nsi_tracing.h"
#include "nsi_fiber_session.h"

#define NSI_FIBER_STACK_SIZE (8U * 1024U * 1024U)

struct fiber_stack {
	void *addr;
	size_t size;
	size_t guard;
	size_t reserve;
	size_t prefix;
	size_t page;
};

struct fiber_probe {
	struct fiber_stack stack;
	void *caller;
	bool ready;
	uint32_t error;
};

static struct nsi_coro_ctx host_ctx;
static struct nsi_coro_ctx *current = &host_ctx;
static struct nsi_coro_ctx *deferred_ctx;
static void *deferred_allocation;
static DWORD owner;
static bool owner_set;
static bool deleting;
static bool cleanup_started;
static struct nsi_coro_ctx cleanup_state;
static struct nsi_coro_ctx *children;
static struct nsi_fiber_session default_session;
static struct nsi_fiber_session *domain_session;
static size_t stack_commit;
static size_t stack_guard;

static void save_state(struct nsi_coro_ctx *ctx)
{
	ctx->err = errno;
	ctx->mxcsr = _mm_getcsr();
	if (fegetenv(&ctx->fenv) != 0) {
		nsi_print_error_and_exit("%s: cannot save floating point environment\n", __func__);
	}
}

static void restore_state(struct nsi_coro_ctx *ctx)
{
	if (fesetenv(&ctx->fenv) != 0) {
		nsi_print_error_and_exit("%s: cannot restore floating point environment\n",
					 __func__);
	}
	/* Keep SSE state explicit, including when the borrowed root lacks FP switching. */
	_mm_setcsr(ctx->mxcsr);
	errno = ctx->err;
}

static void check_owner(void)
{
	if (!owner_set || deleting || GetCurrentThreadId() != owner ||
	    IsThreadAFiber() == FALSE || GetCurrentFiber() != current->fiber ||
	    domain_session->active != &host_ctx) {
		nsi_print_error_and_exit("%s: fiber context used outside its owner thread\n",
					 __func__);
	}
}

static void attach_root(void)
{
	if (owner_set) {
		return;
	}

	/* Pthread NCT attaches on CPU, never during its HW-side nct_init/reset. */
	save_state(&host_ctx);
	if (domain_session == NULL) {
		nsi_fiber_session_open(&default_session);
		domain_session = &default_session;
	}
	nsi_fiber_session_join(domain_session);
	owner = GetCurrentThreadId();
	host_ctx.fiber = domain_session->root;
	host_ctx.domain = &host_ctx;
	host_ctx.ready = true;
	owner_set = true;
	restore_state(&host_ctx);
}

void nsi_fiber_domain_bind(struct nsi_fiber_session *session)
{
	if (owner_set || children != NULL || deferred_ctx != NULL) {
		nsi_print_error_and_exit("%s: cannot bind an attached domain\n", __func__);
	}
	domain_session = session;
}

bool nsi_fiber_domain_is_root(void)
{
	return owner_set && GetCurrentThreadId() == owner && current == &host_ctx &&
		IsThreadAFiber() != FALSE && GetCurrentFiber() == host_ctx.fiber;
}

void nsi_fiber_domain_enter(void)
{
	attach_root();
	if (!nsi_fiber_domain_is_root() || deleting) {
		nsi_print_error_and_exit("%s: grant requires the owner root\n", __func__);
	}
	nsi_fiber_session_enter(domain_session, &host_ctx);
}

void nsi_fiber_domain_leave(void)
{
	check_owner();
	if (current != &host_ctx) {
		nsi_print_error_and_exit("%s: grant release requires the root\n", __func__);
	}
	nsi_fiber_session_leave(domain_session, &host_ctx);
}

size_t nsi_fiber_domain_contexts(void)
{
	size_t count = 0U;

	if (owner_set) {
		check_owner();
	}
	for (struct nsi_coro_ctx *ctx = children; ctx != NULL; ctx = ctx->next) {
		count++;
	}
	return count;
}

void nsi_fiber_domain_detach(void)
{
	if (!nsi_fiber_domain_is_root() || children != NULL || deferred_ctx != NULL ||
	    deleting || cleanup_started) {
		nsi_print_error_and_exit("%s: cannot detach a live or unselected domain\n",
					 __func__);
	}
	nsi_fiber_session_drop(domain_session);
	if (domain_session == &default_session) {
		nsi_fiber_session_close(domain_session);
		domain_session = NULL;
	}
	owner_set = false;
	owner = 0U;
	host_ctx = (struct nsi_coro_ctx){0};
	current = &host_ctx;
}

void nsi_fiber_domain_exit_begin(void)
{
	if (!owner_set) {
		return;
	}
	/* Invalid callers cannot safely recurse through normal domain cleanup. */
	if (deleting || GetCurrentThreadId() != owner || IsThreadAFiber() == FALSE ||
	    GetCurrentFiber() != current->fiber ||
	    (domain_session->active != NULL && domain_session->active != &host_ctx)) {
		ExitProcess(1U);
	}
	if (current == &host_ctx && domain_session->active == NULL) {
		nsi_fiber_domain_enter();
	} else {
		check_owner();
	}
	if (current == &host_ctx && !cleanup_started) {
		save_state(&cleanup_state);
		cleanup_started = true;
	}
}

void nsi_fiber_domain_exit_end(void)
{
	if (owner_set) {
		bool restore = cleanup_started;

		nsi_fiber_domain_leave();
		cleanup_started = false;
		nsi_fiber_domain_detach();
		if (restore) {
			restore_state(&cleanup_state);
		}
	}
}

void nsi_coro_ctx_reset(void)
{
	if (owner_set || deferred_ctx != NULL || children != NULL || cleanup_started) {
		nsi_print_error_and_exit("%s: cannot reset an attached fiber domain\n", __func__);
	}
	host_ctx = (struct nsi_coro_ctx){0};
	current = &host_ctx;
	stack_commit = 0U;
	stack_guard = 0U;
}

static bool bad_stack(void)
{
	SetLastError(ERROR_INVALID_DATA);
	return false;
}

static bool read_stack(struct fiber_stack *stack)
{
	MEMORY_BASIC_INFORMATION region;
	SYSTEM_INFO info;
	ULONG_PTR low;
	ULONG_PTR top;
	uintptr_t allocation;
	uintptr_t bottom = 0U;
	enum { PREFIX, GUARD, WRITABLE } phase = PREFIX;

	GetCurrentThreadStackLimits(&low, &top);
	GetSystemInfo(&info);
	if (VirtualQuery((void *)low, &region, sizeof(region)) != sizeof(region)) {
		return false;
	}
	allocation = (uintptr_t)region.AllocationBase;
	if (allocation == 0U || allocation != low || top <= allocation ||
	    info.dwPageSize == 0U || top % info.dwPageSize != 0U ||
	    allocation % info.dwPageSize != 0U) {
		return bad_stack();
	}

	*stack = (struct fiber_stack){.reserve = top - allocation, .page = info.dwPageSize};
	for (uintptr_t addr = allocation; addr < top; addr += region.RegionSize) {
		if (VirtualQuery((void *)addr, &region, sizeof(region)) != sizeof(region)) {
			return false;
		}
		if (region.Type != MEM_PRIVATE || region.AllocationBase != (void *)allocation ||
		    region.BaseAddress != (void *)addr || region.RegionSize == 0U ||
		    region.RegionSize % info.dwPageSize != 0U || region.RegionSize > top - addr) {
			return bad_stack();
		}
		if (phase == PREFIX && (region.State == MEM_RESERVE ||
		    (region.State == MEM_COMMIT && region.Protect == PAGE_NOACCESS))) {
			stack->prefix += region.RegionSize;
		} else if (phase != WRITABLE && region.State == MEM_COMMIT &&
			   region.Protect == (PAGE_READWRITE | PAGE_GUARD)) {
			stack->guard += region.RegionSize;
			phase = GUARD;
		} else if (phase != PREFIX && region.State == MEM_COMMIT &&
			   region.Protect == PAGE_READWRITE) {
			if (phase == GUARD) {
				bottom = addr;
			}
			phase = WRITABLE;
		} else {
			return bad_stack();
		}
	}
	if (phase != WRITABLE || stack->guard == 0U ||
	    (uintptr_t)&region < bottom || (uintptr_t)&region >= top) {
		return bad_stack();
	}
	stack->addr = (void *)bottom;
	stack->size = top - bottom;
	return true;
}

static VOID WINAPI probe_entry(void *arg)
{
	struct fiber_probe *probe = arg;

	probe->ready = read_stack(&probe->stack);
	probe->error = GetLastError();
	SwitchToFiber(probe->caller);
	nsi_print_error_and_exit("%s: fiber probe resumed\n", __func__);
}

static void discover_commit(void)
{
	SYSTEM_INFO info;
	struct fiber_probe probe = {.caller = current->fiber};
	void *fiber;

	save_state(current);
	GetSystemInfo(&info);
	fiber = CreateFiberEx(info.dwPageSize, NSI_FIBER_STACK_SIZE, FIBER_FLAG_FLOAT_SWITCH,
			      probe_entry, &probe);
	if (fiber == NULL) {
		nsi_print_error_and_exit("%s: cannot create stack probe (Windows error %lu)\n",
					 __func__, GetLastError());
	}
	SwitchToFiber(fiber);
	check_owner();
	DeleteFiber(fiber);
	restore_state(current);
	if (!probe.ready || probe.stack.reserve != NSI_FIBER_STACK_SIZE ||
	    probe.stack.guard >= NSI_FIBER_STACK_SIZE ||
	    probe.stack.page >= NSI_FIBER_STACK_SIZE - probe.stack.guard) {
		nsi_print_error_and_exit("%s: unsupported fiber guard geometry "
					 "(Windows error %u)\n", __func__, probe.error);
	}
	/* Measure the fiber allocator's guard, not the CPU pthread's guard hint.
	 * Full commitment removes the guard. Validate the resulting child independently.
	 */
	stack_guard = probe.stack.guard;
	stack_commit = NSI_FIBER_STACK_SIZE - stack_guard;
}

static bool known_context(struct nsi_coro_ctx *ctx)
{
	if (ctx == &host_ctx) {
		return true;
	}
	for (struct nsi_coro_ctx *child = children; child != NULL; child = child->next) {
		if (ctx == child) {
			return true;
		}
	}
	return false;
}

void nsi_coro_ctx_free(struct nsi_coro_ctx *ctx)
{
	check_owner();
	if (ctx == NULL || ctx == &host_ctx || ctx == current) {
		nsi_print_error_and_exit("%s: cannot delete root or selected fiber\n", __func__);
	}
	if (ctx->fiber != NULL) {
		struct nsi_coro_ctx caller_state;

		/* FLS cleanup can change live state. Keep the saved switch state separate. */
		if (ctx->domain != &host_ctx || !known_context(ctx)) {
			nsi_print_error_and_exit("%s: foreign fiber context\n", __func__);
		}
		save_state(&caller_state);
		deleting = true;
		DeleteFiber(ctx->fiber);
		deleting = false;
		ctx->fiber = NULL;
		ctx->terminal = true;
		if (ctx->prev != NULL) {
			ctx->prev->next = ctx->next;
		} else {
			children = ctx->next;
		}
		if (ctx->next != NULL) {
			ctx->next->prev = ctx->prev;
		}
		ctx->prev = NULL;
		ctx->next = NULL;
		restore_state(&caller_state);
	}
}

static void after_switch(void)
{
	check_owner();
	if (deferred_ctx != NULL) {
		struct nsi_coro_ctx *ctx = deferred_ctx;
		void *allocation = deferred_allocation;

		deferred_ctx = NULL;
		deferred_allocation = NULL;
		nsi_coro_ctx_free(ctx);
		free(allocation);
	}
	restore_state(current);
}

void nsi_coro_ctx_switch(struct nsi_coro_ctx *to, bool final)
{
	struct nsi_coro_ctx *from = current;

	check_owner();
	if (to == NULL || to == current || !known_context(to) || to->domain != &host_ctx ||
	    to->fiber == NULL || to->terminal || !to->ready) {
		nsi_print_error_and_exit("%s: invalid or terminal fiber target\n", __func__);
	}
	save_state(from);
	from->terminal = final;
	current = to;
	SwitchToFiber(to->fiber);
	if (final) {
		nsi_print_error_and_exit("%s: final fiber resumed\n", __func__);
	}
	after_switch();
}

static VOID WINAPI trampoline(void *arg)
{
	struct nsi_coro_ctx *ctx = arg;
	struct fiber_stack stack;

	after_switch();
	if (read_stack(&stack) && stack.reserve == NSI_FIBER_STACK_SIZE &&
	    stack.guard == stack_guard && stack.prefix == 0U && stack.size == stack_commit &&
	    stack.size > stack.page) {
		ctx->stack_addr = stack.addr;
		ctx->stack_size = stack.size;
		ctx->ready = true;
	} else {
		ctx->stack_error = GetLastError();
	}
	/* Suspend bootstrap frames before arch_new_thread can publish/write the sentinel.
	 * No native-stack fill is performed: it would overwrite these live frames.
	 */
	restore_state(ctx);
	nsi_coro_ctx_switch(ctx->creator, false);
	ctx->creator = NULL;
	ctx->entry(ctx->arg);
	nsi_print_error_and_exit("%s: embedded coroutine returned\n", __func__);
}

void nsi_coro_ctx_create(struct nsi_coro_ctx *ctx, void (*entry)(void *), void *arg)
{
	struct nsi_coro_ctx *creator;

#ifndef NSI_NCE_BACKEND_COROUTINES
	if (!owner_set) {
		nsi_fiber_domain_enter();
	}
#endif
	check_owner();
	creator = current;
	if (creator->terminal || ctx->fiber != NULL) {
		nsi_print_error_and_exit("%s: invalid fiber creation\n", __func__);
	}
	save_state(creator);
	if (stack_commit == 0U) {
		discover_commit();
	}
	*ctx = (struct nsi_coro_ctx){.creator = creator, .domain = &host_ctx,
		.entry = entry, .arg = arg, .fenv = creator->fenv, .mxcsr = creator->mxcsr};
	ctx->fiber = CreateFiberEx(stack_commit, NSI_FIBER_STACK_SIZE, FIBER_FLAG_FLOAT_SWITCH,
				  trampoline, ctx);
	if (ctx->fiber == NULL) {
		nsi_print_error_and_exit("%s: cannot create fiber (Windows error %lu)\n",
					 __func__, GetLastError());
	}
	ctx->next = children;
	if (children != NULL) {
		children->prev = ctx;
	}
	children = ctx;
	restore_state(creator);
	/* Bootstrap is not yet a published ready context. */
	save_state(creator);
	current = ctx;
	SwitchToFiber(ctx->fiber);
	after_switch();
	if (!ctx->ready) {
		nsi_print_error_and_exit("%s: fiber stack handshake failed (Windows error %u)\n",
					 __func__, ctx->stack_error);
	}
}

void nsi_coro_ctx_stack(struct nsi_coro_ctx *ctx, void **addr, unsigned long *size)
{
	check_owner();
	if (!known_context(ctx) || ctx->domain != &host_ctx ||
	    !ctx->ready || ctx->fiber == NULL || ctx->terminal) {
		nsi_print_error_and_exit("%s: fiber stack is unavailable\n", __func__);
	}
	*addr = ctx->stack_addr;
	*size = (unsigned long)ctx->stack_size;
}

struct nsi_coro_ctx *nsi_coro_ctx_current(void)
{
	check_owner();
	return current;
}

void nsi_coro_ctx_defer_free(struct nsi_coro_ctx *ctx, void *allocation)
{
	check_owner();
	if (ctx != current || ctx == &host_ctx || deferred_ctx != NULL) {
		nsi_print_error_and_exit("%s: invalid deferred fiber release\n", __func__);
	}
	deferred_ctx = ctx;
	deferred_allocation = allocation;
}
