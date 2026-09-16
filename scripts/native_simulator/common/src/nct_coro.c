/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Native simulator CPU threading emulation, stackful coroutine backend.
 *
 * This is an alternative to nct.c. Instead of one host thread per embedded
 * thread, each embedded thread gets a coroutine on the one host thread which
 * also runs the HW models.
 *
 * It makes no scheduling decision of its own. The embedded OS says which
 * thread should run next, exactly as it does with the host thread backend,
 * and this only performs the switch.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "nct_coro.h"
#include "nct_if.h"
#include "nsi_config.h"
#include "nsi_context_internal.h"
#include "nsi_tracing.h"

#if NSI_N_CPUS > 1
#error "The coroutine backend supports one embedded CPU per runner"
#endif

#define NCT_ALLOC_CHUNK_SIZE 64

enum nct_thread_state {
	NCT_THREAD_UNUSED,
	NCT_THREAD_READY,
	NCT_THREAD_ABORTING,
	NCT_THREAD_ABORTED,
	NCT_THREAD_FAILED,
};

struct nct_status;

struct nct_thread {
	struct nct_status *owner;
	nsi_context_t context;
	nsi_context_stack_t stack;
	void *payload;
	char *name;
	int index;
	int unique_id;
	enum nct_thread_state state;
};

struct nct_status {
	struct nct_thread **threads;
	size_t capacity;
	int creation_count;
	int current;
	void (*entry)(void *payload);
	struct nct_thread *deferred;
	bool terminating;
};

static struct nct_status *debug_instance;

static struct nct_thread *get_thread(struct nct_status *status, int index)
{
	if (status == NULL || index < 0 || (size_t)index >= status->capacity) {
		return NULL;
	}
	return status->threads[index];
}

static int grow_table(struct nct_status *status)
{
	size_t old_capacity = status->capacity;
	size_t new_capacity = old_capacity + NCT_ALLOC_CHUNK_SIZE;
	struct nct_thread **threads = realloc(status->threads,
					      new_capacity * sizeof(*threads));

	if (threads == NULL) {
		return -1;
	}
	memset(threads + old_capacity, 0, (new_capacity - old_capacity) * sizeof(*threads));
	status->threads = threads;
	status->capacity = new_capacity;
	return 0;
}

static void release_thread(struct nct_thread *thread)
{
	if (thread == NULL) {
		return;
	}
	if (nsi_context_destroy_bound(&thread->context, &thread->stack) != NSI_CONTEXT_OK) {
		nsi_print_error_and_exit("Cannot reclaim a live NCT context\n");
	}
	free(thread->name);
	free(thread);
}

/*
 * A thread which aborted itself cannot free its own stack while it is still
 * running on it, so the switch away leaves it here for whoever runs next.
 */
static void reap_deferred(struct nct_status *status)
{
	struct nct_thread *thread = status->deferred;

	if (thread == NULL) {
		return;
	}
	status->deferred = NULL;
	status->threads[thread->index] = NULL;
	release_thread(thread);
}

static void thread_entry(void *argument)
{
	struct nct_thread *thread = argument;
	struct nct_status *status = thread->owner;

	reap_deferred(status);
	status->current = thread->index;
	status->entry(thread->payload);
	thread->state = NCT_THREAD_FAILED;
	status->current = -1;
	(void)nsi_context_terminate_bound();
}

void *nct_init(void (*entry)(void *))
{
	struct nct_status *status;

	if (entry == NULL) {
		return NULL;
	}
	if (nsi_context_bind_default() != NSI_CONTEXT_OK) {
		return NULL;
	}

	status = calloc(1, sizeof(*status));
	if (status == NULL) {
		return NULL;
	}
	status->entry = entry;
	status->current = -1;
	if (grow_table(status) != 0) {
		free(status);
		return NULL;
	}
	debug_instance = status;
	return status;
}

int nct_new_thread(void *this_arg, void *payload)
{
	struct nct_status *status = this_arg;
	struct nct_thread *thread;
	int index;

	if (status == NULL || status->terminating) {
		return -1;
	}
	/* Indices are deliberately never reused during an NCT lifetime. */
	index = status->creation_count;
	if ((size_t)index >= status->capacity && grow_table(status) != 0) {
		return -1;
	}
	thread = calloc(1, sizeof(*thread));
	if (thread == NULL) {
		return -1;
	}
	thread->owner = status;
	thread->payload = payload;
	thread->index = index;
	thread->unique_id = status->creation_count++;
	thread->state = NCT_THREAD_READY;
	if (nsi_context_create_bound(NSI_CONTEXT_ZEPHYR_THREAD, thread_entry, thread,
				     &thread->context, &thread->stack) != NSI_CONTEXT_OK) {
		status->creation_count--;
		free(thread);
		return -1;
	}
	status->threads[index] = thread;
	return index;
}

void nct_swap_threads(void *this_arg, int next_index)
{
	struct nct_status *status = this_arg;
	struct nct_thread *next = get_thread(status, next_index);
	struct nct_thread *current = get_thread(status, status != NULL ? status->current : -1);

	if (status == NULL || next == NULL || next->state != NCT_THREAD_READY) {
		return;
	}
	status->current = next_index;
	if (current == NULL) {
		(void)nsi_context_abandon_bound(next->context);
		return;
	}
	if (current == next) {
		return;
	}
	if (current->state == NCT_THREAD_ABORTING) {
		current->state = NCT_THREAD_ABORTED;
		status->deferred = current;
		(void)nsi_context_abandon_bound(next->context);
		return;
	}
	if (nsi_context_switch_bound(next->context) == NSI_CONTEXT_OK) {
		status->current = current->index;
		reap_deferred(status);
	}
}

void nct_first_thread_start(void *this_arg, int next_index)
{
	struct nct_status *status = this_arg;
	struct nct_thread *next = get_thread(status, next_index);

	if (status == NULL || status->current != -1 || next == NULL ||
	    next->state != NCT_THREAD_READY) {
		return;
	}
	status->current = next_index;
	/* The bootstrap context is abandoned here and never resumes. */
	(void)nsi_context_abandon_bound(next->context);
}

void nct_abort_thread(void *this_arg, int index)
{
	struct nct_status *status = this_arg;
	struct nct_thread *thread = get_thread(status, index);

	if (thread == NULL || thread->state == NCT_THREAD_ABORTED ||
	    thread->state == NCT_THREAD_FAILED) {
		return;
	}
	if (index == status->current) {
		thread->state = NCT_THREAD_ABORTING;
		return;
	}
	thread->state = NCT_THREAD_ABORTED;
	status->threads[index] = NULL;
	release_thread(thread);
}

int nct_get_unique_thread_id(void *this_arg, int index)
{
	struct nct_thread *thread = get_thread(this_arg, index);

	return thread != NULL ? thread->unique_id : -1;
}

int nct_thread_name_set(void *this_arg, int index, const char *name)
{
	struct nct_thread *thread = get_thread(this_arg, index);
	char *copy;

	if (thread == NULL || name == NULL) {
		return EINVAL;
	}
	copy = malloc(strlen(name) + 1);
	if (copy == NULL) {
		return ENOMEM;
	}
	strcpy(copy, name);
	free(thread->name);
	thread->name = copy;
	return 0;
}

void nct_get_thread_stack(void *this_arg, int index, void **stack_addr,
			  unsigned long *stack_size)
{
	struct nct_thread *thread = get_thread(this_arg, index);

	if (stack_addr == NULL || stack_size == NULL) {
		return;
	}
	if (thread == NULL) {
		*stack_addr = NULL;
		*stack_size = 0;
		return;
	}
	*stack_addr = thread->stack.usable_begin;
	*stack_size = (unsigned long)thread->stack.usable_size;
}

void nct_clean_up(void *this_arg)
{
	struct nct_status *status = this_arg;
	size_t index;

	if (status == NULL || status->terminating) {
		return;
	}
	status->terminating = true;
	reap_deferred(status);
	for (index = 0; index < status->capacity; index++) {
		release_thread(status->threads[index]);
		status->threads[index] = NULL;
	}
	free(status->threads);
	if (debug_instance == status) {
		debug_instance = NULL;
	}
	free(status);
}

void *nct_coro_debug_instance(void)
{
	return debug_instance;
}

/*
 * Accessors for a debugger. Embedded threads are not host threads, so a
 * debugger cannot list them by itself. These read state without changing it.
 */
int nct_coro_debug_thread_count(void *this_arg)
{
	struct nct_status *status = this_arg;

	return status != NULL ? status->creation_count : 0;
}

int nct_coro_debug_current_thread(void *this_arg)
{
	struct nct_status *status = this_arg;

	return status != NULL ? status->current : -1;
}

int nct_coro_debug_thread_present(void *this_arg, int index)
{
	return get_thread(this_arg, index) != NULL;
}

const char *nct_coro_debug_thread_name(void *this_arg, int index)
{
	struct nct_thread *thread = get_thread(this_arg, index);

	return (thread != NULL && thread->name != NULL) ? thread->name : "";
}

int nct_coro_debug_thread_state(void *this_arg, int index)
{
	struct nct_thread *thread = get_thread(this_arg, index);

	return thread != NULL ? (int)thread->state : (int)NCT_THREAD_UNUSED;
}

nsi_context_t nct_coro_debug_thread_context(void *this_arg, int index)
{
	struct nct_thread *thread = get_thread(this_arg, index);

	return thread != NULL ? thread->context : 0;
}

void *nct_coro_debug_thread_stack_begin(void *this_arg, int index)
{
	struct nct_thread *thread = get_thread(this_arg, index);

	return thread != NULL ? thread->stack.usable_begin : NULL;
}

size_t nct_coro_debug_thread_stack_size(void *this_arg, int index)
{
	struct nct_thread *thread = get_thread(this_arg, index);

	return thread != NULL ? thread->stack.usable_size : 0;
}
