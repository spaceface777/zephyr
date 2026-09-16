/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The context service the runner provides for itself.
 *
 * It implements nsi_context_if_t on top of the nsi_coro switch primitive and
 * guard paged stacks obtained from the host. A standalone native simulator
 * executable uses this one, which is what lets the coroutine backend work
 * without anything embedding the runner.
 *
 * A program which embeds the runner may replace it through nsi_context_bind().
 *
 * This service assumes it is used from one host thread. It creates none, and
 * it deliberately calls no threading API, so that a build which uses the
 * coroutine backend contains no threading code at all.
 */

#include <errno.h>
#include <fenv.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nsi_context_if.h"
#include "nsi_coro.h"
#include "nsi_tracing.h"

/*
 * A sanitized build has to be told where each stack is, because the switch
 * below is not one the sanitizer can see. Left unsaid, AddressSanitizer
 * measures from the host stack down to a coroutine stack, decides the bounds
 * it computed are not credible, stops honouring __asan_handle_no_return, and
 * says that false positive reports may follow.
 */
#if defined(__SANITIZE_ADDRESS__)
#define NSI_CONTEXT_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NSI_CONTEXT_ASAN 1
#endif
#endif

#ifdef NSI_CONTEXT_ASAN
/* Declared here so that building this does not need a sanitizer header. */
void __sanitizer_start_switch_fiber(void **fake_stack_save, const void *bottom, size_t size);
void __sanitizer_finish_switch_fiber(void *fake_stack_save, const void **bottom_old,
				     size_t *size_old);
#endif

/*
 * A handle is a process wide creation serial above a slot field. The slot
 * field makes resolving a handle one indexed comparison, while the serial
 * keeps handles from repeating when a slot is reused, so that a stale handle
 * is rejected instead of naming somebody else's context.
 *
 * Slot field 0 names the root; slot field n names contexts[n - 1].
 */
#define SLOT_BITS 24
#define SLOT_MASK ((((nsi_context_t)1) << SLOT_BITS) - 1)
#define MAX_SLOTS ((size_t)SLOT_MASK)
#define SERIAL_LIMIT (UINT64_MAX >> SLOT_BITS)

#define MIN_STACK_SIZE (64 * 1024)

enum ctx_state {
	CTX_RUNNABLE,
	CTX_RUNNING,
	CTX_FINISHED,
	CTX_ABANDONED,
};

struct host_stack {
	void *allocation;
	size_t allocation_size;
	void *usable_begin;
	size_t usable_size;
	bool handed_out;
};

struct host_ctx {
	nsi_context_t handle;
	uint64_t owner;
	nsi_context_role_t role;
	enum ctx_state state;
	nsi_coro_t continuation;
	struct host_stack *stack;
	nsi_context_entry_fn entry;
	void *argument;
	/*
	 * return_to is where this context goes when its entry function returns.
	 * resumed_by is whoever switched into it last, which it needs in order
	 * to record their new continuation. They are not the same thing: a
	 * context which is finishing sets resumed_by on its destination but
	 * must leave return_to alone, or that destination would later try to
	 * return into a context which no longer exists.
	 */
	struct host_ctx *return_to;
	struct host_ctx *resumed_by;
	int saved_errno;
	fenv_t floating_point;
#ifdef NSI_CONTEXT_ASAN
	void *asan_fake_stack;
	const void *asan_stack_bottom;
	size_t asan_stack_size;
#endif
};

static struct {
	bool ready;
	bool root_attached;
	nsi_context_if_t interface;
	struct host_ctx root;
	struct host_ctx **contexts;
	size_t capacity;
	struct host_ctx *current;
	struct host_stack **stacks;
	size_t stack_count;
	size_t stack_capacity;
	bool in_operation;
	uint64_t operation_owner;
	uint64_t next_serial;
} host;

#ifdef NSI_CONTEXT_ASAN
static void asan_before_switch(struct host_ctx *from, const struct host_ctx *to, bool for_good)
{
	/*
	 * A context which will not run again passes no place to save its fake
	 * stack, which is how the sanitizer is told to release it.
	 */
	__sanitizer_start_switch_fiber(for_good ? NULL : &from->asan_fake_stack,
				       to->asan_stack_bottom, to->asan_stack_size);
}

static void asan_after_switch(struct host_ctx *self, struct host_ctx *previous)
{
	const void *bottom = NULL;
	size_t size = 0;

	__sanitizer_finish_switch_fiber(self->asan_fake_stack, &bottom, &size);
	/*
	 * What comes back describes whoever ran before us. That is how the root
	 * learns the bounds of the host stack it was already running on, which
	 * it has no other way to know.
	 */
	if (previous != NULL && previous->asan_stack_bottom == NULL) {
		previous->asan_stack_bottom = bottom;
		previous->asan_stack_size = size;
	}
}
#else
#define asan_before_switch(from, to, for_good) ((void)0)
#define asan_after_switch(self, previous) ((void)0)
#endif

static nsi_context_t new_handle(nsi_context_t slot_field)
{
	uint64_t serial = host.next_serial++;

	if (serial == 0 || serial >= SERIAL_LIMIT) {
		nsi_print_error_and_exit("Ran out of context handles\n");
	}
	return (serial << SLOT_BITS) | slot_field;
}

static void capture(struct host_ctx *context)
{
	context->saved_errno = errno;
	if (fegetenv(&context->floating_point) != 0) {
		nsi_print_error_and_exit("Cannot read the floating point environment\n");
	}
}

static void restore(const struct host_ctx *context)
{
	if (fesetenv(&context->floating_point) != 0) {
		nsi_print_error_and_exit("Cannot restore the floating point environment\n");
	}
	errno = context->saved_errno;
}

static struct host_ctx *resolve(nsi_context_t handle)
{
	nsi_context_t slot;

	if (!host.ready || handle == 0) {
		return NULL;
	}
	slot = handle & SLOT_MASK;
	if (slot == 0) {
		return host.root.handle == handle ? &host.root : NULL;
	}
	if (slot > host.capacity) {
		return NULL;
	}
	struct host_ctx *context = host.contexts[slot - 1];

	return (context != NULL && context->handle == handle) ? context : NULL;
}

void *nsi_context_default_debug_continuation(nsi_context_t handle)
{
	struct host_ctx *context = resolve(handle);

	return context != NULL && context->state == CTX_RUNNABLE ? context->continuation : NULL;
}

static bool stack_in_use(const struct host_stack *stack)
{
	for (size_t i = 0; i < host.capacity; i++) {
		if (host.contexts[i] != NULL && host.contexts[i]->stack == stack) {
			return true;
		}
	}
	return false;
}

static void accept_transfer(nsi_coro_t from)
{
	struct host_ctx *self = host.current;
	struct host_ctx *previous;

	if (self == NULL || self->resumed_by == NULL) {
		nsi_print_error_and_exit("Context switched in from nowhere\n");
	}
	previous = self->resumed_by;
	previous->continuation = from;
	asan_after_switch(self, previous);
	self->state = CTX_RUNNING;
	restore(self);
}

static void entry_returned(struct host_ctx *context)
{
	struct host_ctx *destination = context->return_to;

	capture(context);
	context->state = CTX_FINISHED;
	if (destination == NULL || destination->state != CTX_RUNNABLE) {
		nsi_print_error_and_exit("A context finished with nowhere to return to\n");
	}
	destination->state = CTX_RUNNING;
	/* Deliberately not return_to: see the note on those two fields. */
	destination->resumed_by = context;
	host.current = destination;
	asan_before_switch(context, destination, true);
	(void)nsi_coro_switch(destination->continuation);
	nsi_print_error_and_exit("A finished context was resumed\n");
}

static void context_entry(nsi_coro_t from)
{
	accept_transfer(from);

	struct host_ctx *self = host.current;

	self->entry(self->argument);
	entry_returned(self);
}

void nsi_coro_entry_returned(void)
{
	nsi_print_error_and_exit("A coroutine entry function returned\n");
}

static nsi_context_status_t do_transfer(struct host_ctx *from, struct host_ctx *to, bool abandon)
{
	if (host.current != from || from->state != CTX_RUNNING || from == to ||
	    (to->state != CTX_RUNNABLE && to->state != CTX_RUNNING)) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	/*
	 * Only the root may cross between owners. Two embedded CPUs never
	 * switch into each other directly.
	 */
	if (from->role != NSI_CONTEXT_SIMULATOR_ROOT && to->role != NSI_CONTEXT_SIMULATOR_ROOT &&
	    from->owner != to->owner) {
		return NSI_CONTEXT_WRONG_STATE;
	}

	capture(from);
	from->state = abandon ? CTX_ABANDONED : CTX_RUNNABLE;
	to->state = CTX_RUNNING;
	to->return_to = from;
	to->resumed_by = from;
	host.current = to;
	asan_before_switch(from, to, abandon);

	nsi_coro_t result = nsi_coro_switch(to->continuation);

	if (abandon) {
		nsi_print_error_and_exit("An abandoned context was resumed\n");
	}
	accept_transfer(result);
	return NSI_CONTEXT_OK;
}

static nsi_context_status_t grow_contexts(void)
{
	size_t capacity = host.capacity == 0 ? 16 : 2 * host.capacity;

	if (host.capacity >= MAX_SLOTS) {
		return NSI_CONTEXT_NO_MEMORY;
	}
	if (capacity > MAX_SLOTS) {
		capacity = MAX_SLOTS;
	}

	struct host_ctx **contexts = realloc(host.contexts, capacity * sizeof(*contexts));

	if (contexts == NULL) {
		return NSI_CONTEXT_NO_MEMORY;
	}
	memset(contexts + host.capacity, 0, (capacity - host.capacity) * sizeof(*contexts));
	host.contexts = contexts;
	host.capacity = capacity;
	return NSI_CONTEXT_OK;
}

static struct host_stack *find_stack(nsi_context_stack_t bounds)
{
	for (size_t i = 0; i < host.stack_count; i++) {
		if (host.stacks[i]->usable_begin == bounds.usable_begin &&
		    host.stacks[i]->usable_size == bounds.usable_size) {
			return host.stacks[i];
		}
	}
	return NULL;
}

static nsi_context_status_t attach_root(void *user, nsi_context_t *result)
{
	if (user != &host || result == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (host.root_attached) {
		if (host.current != &host.root) {
			return NSI_CONTEXT_WRONG_STATE;
		}
		*result = host.root.handle;
		return NSI_CONTEXT_OK;
	}
	host.root.handle = new_handle(0);
	host.root.role = NSI_CONTEXT_SIMULATOR_ROOT;
	host.root.state = CTX_RUNNING;
	capture(&host.root);
	host.current = &host.root;
	host.root_attached = true;
	*result = host.root.handle;
	return NSI_CONTEXT_OK;
}

static nsi_context_status_t create(void *user, uint64_t owner, nsi_context_role_t role,
				   nsi_context_stack_t requested, nsi_context_entry_fn entry,
				   void *argument, nsi_context_t *result)
{
	if (user != &host || result == NULL || entry == NULL || requested.usable_begin == NULL ||
	    requested.usable_size == 0 || role == NSI_CONTEXT_SIMULATOR_ROOT) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (!host.root_attached) {
		return NSI_CONTEXT_WRONG_STATE;
	}

	struct host_stack *stack = find_stack(requested);

	if (stack == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (stack_in_use(stack)) {
		return NSI_CONTEXT_WRONG_STATE;
	}

	size_t slot = host.capacity;

	for (size_t i = 0; i < host.capacity; i++) {
		if (host.contexts[i] == NULL) {
			slot = i;
			break;
		}
	}
	if (slot == host.capacity) {
		nsi_context_status_t status = grow_contexts();

		if (status != NSI_CONTEXT_OK) {
			return status;
		}
	}

	struct host_ctx *context = calloc(1, sizeof(*context));

	if (context == NULL) {
		return NSI_CONTEXT_NO_MEMORY;
	}
	context->owner = owner;
	context->role = role;
	context->state = CTX_RUNNABLE;
	context->stack = stack;
	context->entry = entry;
	context->argument = argument;
#ifdef NSI_CONTEXT_ASAN
	context->asan_stack_bottom = stack->usable_begin;
	context->asan_stack_size = stack->usable_size;
#endif
	context->saved_errno = errno;
	if (fegetenv(&context->floating_point) != 0) {
		free(context);
		return NSI_CONTEXT_UNSUPPORTED;
	}

	context->continuation = nsi_coro_create((char *)stack->usable_begin + stack->usable_size,
					       stack->usable_size, context_entry);
	if (context->continuation == NULL) {
		free(context);
		return NSI_CONTEXT_NO_MEMORY;
	}

	context->handle = new_handle((nsi_context_t)slot + 1);
	host.contexts[slot] = context;
	*result = context->handle;
	return NSI_CONTEXT_OK;
}

static nsi_context_status_t switch_to(void *user, nsi_context_t from, nsi_context_t to)
{
	struct host_ctx *source;
	struct host_ctx *destination;

	if (user != &host) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	source = resolve(from);
	destination = resolve(to);
	if (source == NULL || destination == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	return do_transfer(source, destination, false);
}

static nsi_context_status_t abandon_to(void *user, nsi_context_t from, nsi_context_t to)
{
	struct host_ctx *source;
	struct host_ctx *destination;

	if (user != &host) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	source = resolve(from);
	destination = resolve(to);
	if (source == NULL || destination == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	return do_transfer(source, destination, true);
}

static nsi_context_status_t destroy(void *user, nsi_context_t handle)
{
	if (user != &host || handle == 0) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}

	struct host_ctx *context = resolve(handle);

	if (context == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (context == &host.root || context == host.current || context->state == CTX_RUNNING) {
		return NSI_CONTEXT_WRONG_STATE;
	}

	host.contexts[(handle & SLOT_MASK) - 1] = NULL;
	free(context);
	return NSI_CONTEXT_OK;
}

static nsi_context_status_t stack_bounds(void *user, nsi_context_t handle,
					 nsi_context_stack_t *result)
{
	if (user != &host || handle == 0 || result == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}

	struct host_ctx *context = resolve(handle);

	if (context == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (context->stack == NULL) {
		result->usable_begin = NULL;
		result->usable_size = 0;
	} else {
		result->usable_begin = context->stack->usable_begin;
		result->usable_size = context->stack->usable_size;
	}
	return NSI_CONTEXT_OK;
}

static nsi_context_status_t allocate_stack(void *user, size_t size, nsi_context_stack_t *result)
{
	if (user != &host || result == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}

	long page_value = sysconf(_SC_PAGESIZE);

	if (page_value <= 0) {
		return NSI_CONTEXT_UNSUPPORTED;
	}

	size_t page = (size_t)page_value;
	size_t usable = size < MIN_STACK_SIZE ? MIN_STACK_SIZE : size;

	if (usable > SIZE_MAX - page + 1) {
		return NSI_CONTEXT_NO_MEMORY;
	}
	usable = ((usable + page - 1) / page) * page;
	if (usable > SIZE_MAX - 2 * page) {
		return NSI_CONTEXT_NO_MEMORY;
	}

	size_t total = usable + 2 * page;
	/* PROT_NONE pages on both sides, so an overflow or underflow faults. */
	void *allocation = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);

	if (allocation == MAP_FAILED) {
		return NSI_CONTEXT_NO_MEMORY;
	}

	void *usable_begin = (char *)allocation + page;

	if (mprotect(usable_begin, usable, PROT_READ | PROT_WRITE) != 0) {
		munmap(allocation, total);
		return NSI_CONTEXT_NO_MEMORY;
	}

	if (host.stack_count == host.stack_capacity) {
		size_t capacity = host.stack_capacity == 0 ? 16 : 2 * host.stack_capacity;
		struct host_stack **stacks = realloc(host.stacks, capacity * sizeof(*stacks));

		if (stacks == NULL) {
			munmap(allocation, total);
			return NSI_CONTEXT_NO_MEMORY;
		}
		host.stacks = stacks;
		host.stack_capacity = capacity;
	}

	struct host_stack *stack = calloc(1, sizeof(*stack));

	if (stack == NULL) {
		munmap(allocation, total);
		return NSI_CONTEXT_NO_MEMORY;
	}
	stack->allocation = allocation;
	stack->allocation_size = total;
	stack->usable_begin = usable_begin;
	stack->usable_size = usable;
	stack->handed_out = true;
	host.stacks[host.stack_count++] = stack;

	result->usable_begin = usable_begin;
	result->usable_size = usable;
	return NSI_CONTEXT_OK;
}

static nsi_context_status_t release_stack(void *user, nsi_context_stack_t requested)
{
	if (user != &host || requested.usable_begin == NULL || requested.usable_size == 0) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}

	struct host_stack *stack = find_stack(requested);

	if (stack == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (stack_in_use(stack)) {
		return NSI_CONTEXT_WRONG_STATE;
	}

	for (size_t i = 0; i < host.stack_count; i++) {
		if (host.stacks[i] == stack) {
			host.stacks[i] = host.stacks[host.stack_count - 1];
			host.stack_count--;
			break;
		}
	}
	munmap(stack->allocation, stack->allocation_size);
	free(stack);
	return NSI_CONTEXT_OK;
}

static nsi_context_status_t enter_operation(void *user, uint64_t owner, nsi_context_t *result)
{
	if (user != &host || result == NULL || owner == 0) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (!host.root_attached || host.current != &host.root || host.in_operation) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	host.in_operation = true;
	host.operation_owner = owner;
	*result = host.root.handle;
	return NSI_CONTEXT_OK;
}

static nsi_context_status_t leave_operation(void *user, uint64_t owner)
{
	if (user != &host || owner == 0) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (!host.root_attached || host.current != &host.root || !host.in_operation ||
	    host.operation_owner != owner) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	host.in_operation = false;
	host.operation_owner = 0;
	return NSI_CONTEXT_OK;
}

const nsi_context_if_t *nsi_context_default_if(void)
{
	if (!host.ready) {
		host.next_serial = 1;
		host.interface.header.version = NSI_CONTEXT_ABI_VERSION;
		host.interface.header.size = sizeof(nsi_context_if_t);
		host.interface.user = &host;
		host.interface.attach_root = attach_root;
		host.interface.create = create;
		host.interface.switch_to = switch_to;
		host.interface.abandon_to = abandon_to;
		host.interface.destroy = destroy;
		host.interface.stack_bounds = stack_bounds;
		host.interface.allocate_stack = allocate_stack;
		host.interface.release_stack = release_stack;
		host.interface.enter_operation = enter_operation;
		host.interface.leave_operation = leave_operation;
		host.ready = true;
	}
	return &host.interface;
}
