/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "nsi_coro_ctx.h"
#include "nsi_tracing.h"

#ifdef NSI_CORO_ASAN
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save, const void **bottom_old,
				     size_t *size_old);
#endif

#ifdef _WIN32
/* The Windows runner is C-only, so no C++ exception globals exist. */
static struct cxa_eh_globals *(*const cxa_get_globals)(void);

static int fpenv_save(struct nsi_coro_fpenv *env)
{
	/* fnstenv masks all x87 exceptions, so load the saved environment again. */
	__asm__ volatile("fnstenv %0" : "=m"(env->x87));
	__asm__ volatile("fldenv %0" : : "m"(env->x87));
	__asm__ volatile("stmxcsr %0" : "=m"(env->mxcsr));
	return 0;
}

static int fpenv_restore(const struct nsi_coro_fpenv *env)
{
	__asm__ volatile("fldenv %0" : : "m"(env->x87));
	__asm__ volatile("ldmxcsr %0" : : "m"(env->mxcsr));
	return 0;
}

static size_t page_size(void)
{
	SYSTEM_INFO info;

	GetSystemInfo(&info);
	return info.dwPageSize;
}
#else
struct cxa_eh_globals *__cxa_get_globals(void) __attribute__((weak));
#define cxa_get_globals __cxa_get_globals
#define fpenv_save fegetenv
#define fpenv_restore fesetenv
#endif

static struct nsi_coro_ctx host_ctx;
static struct nsi_coro_ctx *current = &host_ctx;
static struct nsi_coro_ctx *previous;
static struct nsi_coro_ctx *deferred_ctx;
static void *deferred_allocation;
static pthread_t owner;
static bool owner_set;

static void check_owner(void)
{
	pthread_t self = pthread_self();

	if (!owner_set) {
		owner = self;
		owner_set = true;
	} else if (!pthread_equal(owner, self)) {
		nsi_print_error_and_exit("%s: coroutine context migrated between host threads\n",
					 __func__);
	}
}

void nsi_coro_ctx_reset(void)
{
	memset(&host_ctx, 0, sizeof(host_ctx));
	current = &host_ctx;
	previous = NULL;
	deferred_ctx = NULL;
	deferred_allocation = NULL;
	owner_set = false;
}

void nsi_coro_ctx_free(struct nsi_coro_ctx *ctx)
{
	if (ctx->map != NULL) {
#ifdef _WIN32
		if (VirtualFree(ctx->map, 0, MEM_RELEASE) == 0) {
			nsi_print_error_and_exit("%s: cannot release coroutine stack\n", __func__);
		}
#else
		(void)munmap(ctx->map, ctx->map_size);
#endif
		ctx->map = NULL;
	}
}

static void after_switch(nsi_coro_t from)
{
#ifdef NSI_CORO_ASAN
	const void *bottom;
	size_t size;

	__sanitizer_finish_switch_fiber(current->asan_fake_stack, &bottom, &size);
	if (previous->asan_bottom == NULL) {
		previous->asan_bottom = bottom;
		previous->asan_size = size;
	}
#endif
	previous->continuation = from;
	if (deferred_ctx != NULL) {
		nsi_coro_ctx_free(deferred_ctx);
		free(deferred_allocation);
		deferred_ctx = NULL;
		deferred_allocation = NULL;
	}
	if (fpenv_restore(&current->fenv) != 0) {
		nsi_print_error_and_exit("%s: cannot restore floating point environment\n",
					 __func__);
	}
	if (cxa_get_globals != NULL) {
		*cxa_get_globals() = current->eh;
	}
	errno = current->err;
}

void nsi_coro_ctx_switch(struct nsi_coro_ctx *to, bool final)
{
	check_owner();
	if (to == current || to->continuation == NULL) {
		nsi_print_error_and_exit("%s: invalid coroutine context switch\n", __func__);
	}

	current->err = errno;
	if (cxa_get_globals != NULL) {
		current->eh = *cxa_get_globals();
	}
	if (fpenv_save(&current->fenv) != 0) {
		nsi_print_error_and_exit("%s: cannot save floating point environment\n",
					 __func__);
	}

	previous = current;
	current = to;
#ifdef NSI_CORO_ASAN
	__sanitizer_start_switch_fiber(final ? NULL : &previous->asan_fake_stack,
				       to->asan_bottom, to->asan_size);
#else
	(void)final;
#endif
	after_switch(nsi_coro_switch(to->continuation));
}

static void trampoline(nsi_coro_t from)
{
	after_switch(from);
	current->entry(current->arg);
	nsi_print_error_and_exit("%s: embedded coroutine returned\n", __func__);
}

void nsi_coro_ctx_create(struct nsi_coro_ctx *ctx, void (*entry)(void *), void *arg)
{
#ifdef _WIN32
	size_t page = page_size();

	/*
	 * Commit the whole stack: Zephyr can write a stack sentinel at its lowest
	 * address. The reserved pages at both ends stay inaccessible.
	 */
	ctx->map_size = NSI_CORO_STACK_SIZE + 2 * page;
	ctx->map = VirtualAlloc(NULL, ctx->map_size, MEM_RESERVE, PAGE_NOACCESS);
	if ((ctx->map == NULL) ||
	    (VirtualAlloc((char *)ctx->map + page, NSI_CORO_STACK_SIZE, MEM_COMMIT,
			  PAGE_READWRITE) == NULL)) {
		nsi_print_error_and_exit("%s: cannot allocate coroutine stack "
					 "(Windows error %lu)\n", __func__, GetLastError());
	}
#else
	size_t page = (size_t)sysconf(_SC_PAGESIZE);

	ctx->map_size = NSI_CORO_STACK_SIZE + 2 * page;
	ctx->map = mmap(NULL, ctx->map_size, PROT_NONE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if ((ctx->map == MAP_FAILED) ||
	    (mprotect((char *)ctx->map + page, NSI_CORO_STACK_SIZE,
		      PROT_READ | PROT_WRITE) != 0)) {
		nsi_print_error_and_exit("%s: cannot allocate coroutine stack\n", __func__);
	}
#endif

	ctx->continuation = nsi_coro_create((char *)ctx->map + page + NSI_CORO_STACK_SIZE,
					    trampoline);
	ctx->err = 0;
	if (fpenv_save(&ctx->fenv) != 0) {
		nsi_print_error_and_exit("%s: cannot read floating point environment\n",
					 __func__);
	}
	ctx->entry = entry;
	ctx->arg = arg;
#ifdef NSI_CORO_ASAN
	ctx->asan_fake_stack = NULL;
	ctx->asan_bottom = (char *)ctx->map + page;
	ctx->asan_size = NSI_CORO_STACK_SIZE;
#endif
}

void nsi_coro_ctx_stack(struct nsi_coro_ctx *ctx, void **addr, unsigned long *size)
{
#ifdef _WIN32
	size_t page = page_size();
#else
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
#endif

	*addr = (char *)ctx->map + page;
	*size = NSI_CORO_STACK_SIZE;
}

struct nsi_coro_ctx *nsi_coro_ctx_current(void)
{
	return current;
}

void nsi_coro_ctx_defer_free(struct nsi_coro_ctx *ctx, void *allocation)
{
	if (deferred_ctx != NULL) {
		nsi_print_error_and_exit("%s: coroutine context already pending release\n",
					 __func__);
	}
	deferred_ctx = ctx;
	deferred_allocation = allocation;
}
