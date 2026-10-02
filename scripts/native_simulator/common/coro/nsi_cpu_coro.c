/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * CPU start/stop emulation (NCE, see nce_if.h) and CPU thread emulation
 * (NCT, see nct_if.h) implemented with stackful coroutines.
 *
 * This is an alternative to nce.c and nct.c, selected with
 * NSI_CPU_BACKEND=coroutines. Instead of one host thread per embedded CPU and
 * per embedded thread, which hand over execution with semaphores, everything
 * runs on the single host thread which runs the HW models, and each embedded
 * CPU and thread is a coroutine with its own stack. A hand-over is a
 * user-space register switch (nsi_coro.S) instead of a host thread wake up.
 *
 * As with the host thread implementation, this makes no scheduling decision:
 * the embedded OS says which of its threads runs next, and the HW models say
 * when a CPU is woken.
 *
 * The state which a host thread has for itself, and a coroutine would
 * otherwise share, is switched with the context: errno and the floating
 * point environment.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* For MAP_ANONYMOUS in glibc */
#endif

#include <errno.h>
#include <fenv.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "nce_if.h"
#include "nct_if.h"
#include "nsi_config.h"
#include "nsi_coro.h"
#include "nsi_main.h"
#include "nsi_tracing.h"

/*
 * Size of each coroutine stack. Only address space is reserved: pages are
 * committed on first use, like for a host thread.
 */
#ifndef NSI_CORO_STACK_SIZE
#define NSI_CORO_STACK_SIZE (8 * 1024 * 1024)
#endif

#if defined(__SANITIZE_ADDRESS__)
#define NSI_CORO_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NSI_CORO_ASAN 1
#endif
#endif

#ifdef NSI_CORO_ASAN
/* AddressSanitizer needs to be told about stack switches (sanitizer/common_interface_defs.h) */
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save, const void **bottom_old,
				     size_t *size_old);
#endif

/*
 * The C++ runtime's per thread exception state (Itanium C++ ABI, 2.2.2): the stack of
 * caught exceptions and the count of uncaught ones. Only present in C++ programs.
 */
struct cxa_eh_globals {
	void *caught_exceptions;
	unsigned int uncaught_exceptions;
};
struct cxa_eh_globals *__cxa_get_globals(void) __attribute__((weak));

struct ctx {
	struct cxa_eh_globals eh; /* Saved C++ exception state */
	nsi_coro_t k; /* Saved continuation while suspended */
	void *map; /* Stack mapping, NULL for the host thread's own stack */
	size_t map_size;
	int err; /* Saved errno */
	fenv_t fenv; /* Saved floating point environment */
	void (*entry)(void *arg); /* Called on the first switch into this context */
	void *arg;
#ifdef NSI_CORO_ASAN
	void *asan_fake_stack;
	const void *asan_bottom;
	size_t asan_size;
#endif
};

static struct ctx host_ctx; /* The host thread's own stack, where the HW models run */
static struct ctx *cur = &host_ctx; /* The running context */
static struct ctx *prev; /* The context which switched to cur */
/* A finished context, released (with zombie_mem) by the next context, once off its stack */
static struct ctx *zombie;
static void *zombie_mem;

static void ctx_free(struct ctx *c)
{
	if (c->map != NULL) {
		(void)munmap(c->map, c->map_size);
		c->map = NULL;
	}
}

/* Run on arrival at a context, right after the switch */
static void after_switch(nsi_coro_t from)
{
#ifdef NSI_CORO_ASAN
	const void *bottom;
	size_t size;

	__sanitizer_finish_switch_fiber(cur->asan_fake_stack, &bottom, &size);
	if (prev->asan_bottom == NULL) { /* Learn the host thread stack bounds */
		prev->asan_bottom = bottom;
		prev->asan_size = size;
	}
#endif
	prev->k = from;
	if (zombie != NULL) {
		ctx_free(zombie);
		free(zombie_mem);
		zombie = NULL;
		zombie_mem = NULL;
	}
	if (fesetenv(&cur->fenv) != 0) {
		nsi_print_error_and_exit("%s: Cannot restore the floating point environment\n",
					 __func__);
	}
	if (__cxa_get_globals != NULL) {
		*__cxa_get_globals() = cur->eh;
	}
	errno = cur->err;
}

/*
 * Switch to context <to>. Returns when something switches back to the caller.
 * <final> tells that the caller will never be resumed.
 */
static void switch_to(struct ctx *to, bool final)
{
	if (to == cur || to->k == NULL) {
		nsi_print_error_and_exit("%s: Programming error, invalid context switch\n",
					 __func__);
	}
	cur->err = errno;
	if (__cxa_get_globals != NULL) {
		cur->eh = *__cxa_get_globals();
	}
	if (fegetenv(&cur->fenv) != 0) {
		nsi_print_error_and_exit("%s: Cannot save the floating point environment\n",
					 __func__);
	}
	prev = cur;
	cur = to;
#ifdef NSI_CORO_ASAN
	__sanitizer_start_switch_fiber(final ? NULL : &prev->asan_fake_stack,
				       to->asan_bottom, to->asan_size);
#else
	(void)final;
#endif
	after_switch(nsi_coro_switch(to->k));
}

static void ctx_trampoline(nsi_coro_t from)
{
	after_switch(from);
	cur->entry(cur->arg);
	nsi_print_error_and_exit("%s: Programming error, an embedded context returned\n",
				 __func__);
}

static void ctx_create(struct ctx *c, void (*entry)(void *), void *arg)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);

	/* A guard page at each end, so overflowing the stack faults */
	c->map_size = NSI_CORO_STACK_SIZE + 2 * page;
	c->map = mmap(NULL, c->map_size, PROT_NONE,
		      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if ((c->map == MAP_FAILED) ||
	    (mprotect((char *)c->map + page, NSI_CORO_STACK_SIZE, PROT_READ | PROT_WRITE) != 0)) {
		nsi_print_error_and_exit("%s: Cannot allocate a stack\n", __func__);
	}
	c->k = nsi_coro_create((char *)c->map + page + NSI_CORO_STACK_SIZE, ctx_trampoline);
	c->err = 0;
	if (fegetenv(&c->fenv) != 0) {
		nsi_print_error_and_exit("%s: Cannot read the floating point environment\n",
					 __func__);
	}
	c->entry = entry;
	c->arg = arg;
#ifdef NSI_CORO_ASAN
	c->asan_fake_stack = NULL;
	c->asan_bottom = (char *)c->map + page;
	c->asan_size = NSI_CORO_STACK_SIZE;
#endif
}

static void ctx_stack(struct ctx *c, void **addr, unsigned long *size)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);

	*addr = (char *)c->map + page;
	*size = NSI_CORO_STACK_SIZE;
}

/*
 * NCE: CPU start/stop emulation
 *
 * Waking a CPU switches to the context which last halted it (the boot context
 * the first time). Halting it switches back to whoever woke it, normally the
 * HW models.
 */

struct nce {
	struct ctx boot; /* Runs the CPU start routine, until the first thread starts */
	struct ctx *sw; /* The context which last halted the CPU */
	struct ctx *waker; /* The context which woke the CPU */
	void (*start_routine)(void);
	bool halted;
	bool terminated;
};

static void boot_entry(void *arg)
{
	struct nce *this = arg;

	this->start_routine();
	nsi_print_error_and_exit("%s: Programming error, the CPU start routine returned\n",
				 __func__);
}

/* One per embedded CPU. Reused round robin if the runner is initialized again. */
static struct nce nce_instances[NSI_N_CPUS];
static int nce_next;

void *nce_init(void)
{
	struct nce *this = &nce_instances[nce_next];

	nce_next = (nce_next + 1) % NSI_N_CPUS;
	*this = (struct nce){.halted = true};
	return this;
}

void nce_boot_cpu(void *this_arg, void (*start_routine)(void))
{
	struct nce *this = this_arg;

	this->start_routine = start_routine;
	ctx_create(&this->boot, boot_entry, this);
	this->sw = &this->boot;
	nce_wake_cpu(this);
}

void nce_halt_cpu(void *this_arg)
{
	struct nce *this = this_arg;

	if (this->halted) {
		nsi_print_error_and_exit("%s: Programming error, this CPU was already halted\n",
					 __func__);
	}
	this->halted = true;
	this->sw = cur;
	switch_to(this->waker, false);
}

void nce_wake_cpu(void *this_arg)
{
	struct nce *this = this_arg;

	if (this->terminated) {
		return;
	}
	if (!this->halted) {
		nsi_print_error_and_exit("%s: Programming error, this CPU was already awake\n",
					 __func__);
	}
	this->halted = false;
	this->waker = cur;
	switch_to(this->sw, false);

	if (this->terminated) {
		/* As with the host thread backend, the exit is completed from here */
		nsi_exit(0);
	}
}

/*
 * When called from the embedded side (the CPU is running), hand control back
 * to whoever woke the CPU, which completes the exit (see nce_wake_cpu()).
 * When called from the HW side (the CPU is halted), release the CPU.
 */
void nce_terminate(void *this_arg)
{
	struct nce *this = this_arg;

	if (this == NULL) {
		return;
	}
	if (!this->halted) {
		this->terminated = true;
		this->halted = true;
		switch_to(this->waker, true);
	}
	ctx_free(&this->boot);
}

int nce_is_cpu_running(void *this_arg)
{
	struct nce *this = this_arg;

	return (this != NULL) && !this->halted;
}

/*
 * NCT: CPU thread emulation
 *
 * Each embedded thread is a context. Swapping threads switches to the next
 * thread's context. As with the host thread backend, thread indexes are not
 * reused.
 */

#define NCT_ALLOC_CHUNK_SIZE 64

struct nct_thread {
	struct ctx c;
	struct nct *owner;
	void *payload;
	bool aborting;
};

struct nct {
	void (*fptr)(void *payload);
	struct nct_thread **threads; /* Indexed by thread index, NULL once released */
	int n_threads; /* Threads created so far */
	int allocated;
	int current; /* Index of the running thread, or -1 */
};

static struct nct_thread *nct_get(struct nct *this, int idx)
{
	if ((idx < 0) || (idx >= this->n_threads) || (this->threads[idx] == NULL)) {
		nsi_print_error_and_exit("%s: Programming error, invalid thread index %i\n",
					 __func__, idx);
	}
	return this->threads[idx];
}

static void nct_release(struct nct *this, int idx)
{
	struct nct_thread *th = this->threads[idx];

	ctx_free(&th->c);
	free(th);
	this->threads[idx] = NULL;
}

static void thread_entry(void *arg)
{
	struct nct_thread *th = arg;

	th->owner->fptr(th->payload);
	nsi_print_error_and_exit("%s: Programming error, an embedded thread returned\n",
				 __func__);
}

/* One per embedded CPU. Reused round robin if the runner is initialized again. */
static struct nct nct_instances[NSI_N_CPUS];
static int nct_next;

void *nct_init(void (*fptr)(void *))
{
	struct nct *this = &nct_instances[nct_next];

	nct_next = (nct_next + 1) % NSI_N_CPUS;
	*this = (struct nct){.fptr = fptr, .current = -1};
	return this;
}

int nct_new_thread(void *this_arg, void *payload)
{
	struct nct *this = this_arg;
	struct nct_thread *th;
	int idx = this->n_threads;

	if (idx == this->allocated) {
		struct nct_thread **threads = realloc(this->threads,
			(this->allocated + NCT_ALLOC_CHUNK_SIZE) * sizeof(struct nct_thread *));

		if (threads == NULL) {
			nsi_print_error_and_exit("%s: Out of memory\n", __func__);
		}
		this->threads = threads;
		this->allocated += NCT_ALLOC_CHUNK_SIZE;
	}
	th = calloc(1, sizeof(struct nct_thread));
	if (th == NULL) {
		nsi_print_error_and_exit("%s: Out of memory\n", __func__);
	}
	th->owner = this;
	th->payload = payload;
	ctx_create(&th->c, thread_entry, th);
	this->threads[idx] = th;
	this->n_threads++;
	return idx;
}

void nct_swap_threads(void *this_arg, int next_allowed_thread_nbr)
{
	struct nct *this = this_arg;
	struct nct_thread *next = nct_get(this, next_allowed_thread_nbr);
	int this_th_nbr = this->current;

	if (next_allowed_thread_nbr == this_th_nbr) {
		return;
	}
	this->current = next_allowed_thread_nbr;

	if ((this_th_nbr >= 0) && this->threads[this_th_nbr]->aborting) {
		/* This thread aborted itself: it is released once we are off its stack */
		zombie_mem = this->threads[this_th_nbr];
		zombie = &this->threads[this_th_nbr]->c;
		this->threads[this_th_nbr] = NULL;
		switch_to(&next->c, true); /* Never returns */
	} else {
		switch_to(&next->c, false);
		this->current = this_th_nbr;
	}
}

void nct_first_thread_start(void *this_arg, int next_allowed_thread_nbr)
{
	struct nct *this = this_arg;

	/* Called from the CPU boot context, which is left for good */
	zombie = cur;
	this->current = next_allowed_thread_nbr;
	switch_to(&nct_get(this, next_allowed_thread_nbr)->c, true);
}

void nct_abort_thread(void *this_arg, int thread_idx)
{
	struct nct *this = this_arg;

	if ((thread_idx < 0) || (thread_idx >= this->n_threads) ||
	    (this->threads[thread_idx] == NULL)) {
		return; /* The thread may have been already aborted before */
	}
	if (thread_idx == this->current) {
		/* Completed when this thread swaps out for the last time */
		this->threads[thread_idx]->aborting = true;
		return;
	}
	nct_release(this, thread_idx);
}

int nct_get_unique_thread_id(void *this_arg, int thread_idx)
{
	(void)this_arg;
	return thread_idx; /* Thread indexes are never reused */
}

int nct_thread_name_set(void *this_arg, int thread_idx, const char *str)
{
	(void)this_arg;
	(void)thread_idx;
	(void)str;
	return 0;
}

void nct_get_thread_stack(void *this_arg, int thread_idx, void **stack_addr,
			  unsigned long *stack_size)
{
	ctx_stack(&nct_get(this_arg, thread_idx)->c, stack_addr, stack_size);
}

void nct_clean_up(void *this_arg)
{
	struct nct *this = this_arg;

	if (this == NULL) {
		return;
	}
	for (int i = 0; i < this->n_threads; i++) {
		if (this->threads[i] != NULL) {
			nct_release(this, i);
		}
	}
	free(this->threads);
	*this = (struct nct){.current = -1};
}
