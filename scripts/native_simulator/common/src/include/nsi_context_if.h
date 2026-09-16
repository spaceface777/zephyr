/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NSI_COMMON_SRC_INCL_NSI_CONTEXT_IF_H
#define NSI_COMMON_SRC_INCL_NSI_CONTEXT_IF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Execution context service used by the coroutine NCT and NCE backends.
 *
 * The service is a mechanism, not a scheduler: it creates contexts, switches
 * between them, and owns their stacks. It keeps no run queue and makes no
 * decision about which context should run next.
 *
 * The runner ships a default implementation, which is what a standalone
 * native simulator executable uses. A program which embeds the runner can
 * supply its own with nsi_context_bind(), for example to share one stack
 * allocator with the rest of the host program, to add sanitizer annotations,
 * or to keep its own accounting.
 *
 * This service provides no thread local storage of any kind. It never replaces
 * the host thread pointer.
 */

#define NSI_CONTEXT_ABI_VERSION 4u

/* Opaque context identifier. Zero is never a valid context. */
typedef uint64_t nsi_context_t;

typedef struct nsi_context_abi_header {
	uint32_t version;
	uint32_t size;
} nsi_context_abi_header_t;

typedef enum nsi_context_status {
	NSI_CONTEXT_OK,
	NSI_CONTEXT_INVALID_ARGUMENT,
	NSI_CONTEXT_ABI_MISMATCH,
	NSI_CONTEXT_WRONG_STATE,
	NSI_CONTEXT_NO_MEMORY,
	NSI_CONTEXT_UNSUPPORTED,
} nsi_context_status_t;

typedef enum nsi_context_role {
	/* The context which runs the HW models and owns the CPU boundary. */
	NSI_CONTEXT_SIMULATOR_ROOT,
	/* The context which first enters the embedded CPU's start routine. */
	NSI_CONTEXT_ZEPHYR_BOOTSTRAP,
	/* One context per embedded thread. */
	NSI_CONTEXT_ZEPHYR_THREAD,
} nsi_context_role_t;

typedef void (*nsi_context_entry_fn)(void *argument);

typedef struct nsi_context_stack {
	void *usable_begin;
	size_t usable_size;
} nsi_context_stack_t;

typedef struct nsi_context_if {
	nsi_context_abi_header_t header;
	void *user;

	/* Adopt the currently running context as the root. */
	nsi_context_status_t (*attach_root)(void *user, nsi_context_t *root);

	nsi_context_status_t (*create)(void *user, uint64_t owner, nsi_context_role_t role,
				       nsi_context_stack_t stack, nsi_context_entry_fn entry,
				       void *argument, nsi_context_t *context);

	/* Switch to `to`, expecting to come back. */
	nsi_context_status_t (*switch_to)(void *user, nsi_context_t from, nsi_context_t to);

	/* Switch to `to` and never resume `from`. */
	nsi_context_status_t (*abandon_to)(void *user, nsi_context_t from, nsi_context_t to);

	nsi_context_status_t (*destroy)(void *user, nsi_context_t context);

	nsi_context_status_t (*stack_bounds)(void *user, nsi_context_t context,
					     nsi_context_stack_t *stack);

	nsi_context_status_t (*allocate_stack)(void *user, size_t size,
					       nsi_context_stack_t *stack);

	nsi_context_status_t (*release_stack)(void *user, nsi_context_stack_t stack);

	/*
	 * Instance operations run on the root context, and so do HW model
	 * callbacks, so attach_root alone cannot tell one from an operation
	 * entered from inside another operation.
	 *
	 * enter_operation fails unless the root is the running context and no
	 * operation is already in progress on this host thread.
	 * leave_operation ends the operation which `owner` entered.
	 */
	nsi_context_status_t (*enter_operation)(void *user, uint64_t owner, nsi_context_t *root);
	nsi_context_status_t (*leave_operation)(void *user, uint64_t owner);
} nsi_context_if_t;

static inline nsi_context_status_t nsi_context_check_abi(const nsi_context_if_t *interface)
{
	if (interface == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (interface->header.version != NSI_CONTEXT_ABI_VERSION ||
	    interface->header.size < sizeof(*interface)) {
		return NSI_CONTEXT_ABI_MISMATCH;
	}
	if (!interface->attach_root || !interface->create || !interface->switch_to ||
	    !interface->abandon_to || !interface->destroy || !interface->stack_bounds ||
	    !interface->allocate_stack || !interface->release_stack ||
	    !interface->enter_operation || !interface->leave_operation) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	return NSI_CONTEXT_OK;
}

/**
 * The implementation the runner provides itself, built on nsi_coro and
 * guard-paged stacks. A standalone executable uses this one.
 */
const nsi_context_if_t *nsi_context_default_if(void);

/**
 * Bind the adapter to a context service. `owner_context` may be zero, in which
 * case the service is asked to adopt the running context as the root.
 *
 * Passing NULL as the interface binds nsi_context_default_if().
 */
nsi_context_status_t nsi_context_bind(const nsi_context_if_t *interface, uint64_t owner,
				      nsi_context_t owner_context, size_t native_stack_size);

nsi_context_status_t nsi_context_unbind(void);

#ifdef __cplusplus
}
#endif

#endif /* NSI_COMMON_SRC_INCL_NSI_CONTEXT_IF_H */
