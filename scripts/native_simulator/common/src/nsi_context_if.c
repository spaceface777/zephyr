/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Adapter between the NCT and NCE coroutine backends and whichever context
 * service is bound.
 *
 * It remembers the bound interface, the owner, and which context is running,
 * so the backends do not each have to carry that around. It adds no policy:
 * every decision about which context runs next is made by the backends, which
 * in turn only do what the embedded OS asked for.
 */

#include <string.h>

#include "nsi_context_internal.h"

/*
 * Stack size for contexts created through this adapter, when the binding did
 * not ask for a particular one. Override at build time if the embedded code
 * needs deeper native stacks.
 */
#ifndef NSI_CONTEXT_DEFAULT_STACK_SIZE
#define NSI_CONTEXT_DEFAULT_STACK_SIZE (1024 * 1024)
#endif

/* Owner id used when the runner binds its own service. */
#define NSI_CONTEXT_DEFAULT_OWNER 1u

static struct {
	const nsi_context_if_t *interface;
	uint64_t owner;
	size_t stack_size;
	nsi_context_t owner_context;
	nsi_context_t active_context;
	nsi_context_terminal_fn terminal_handler;
	void *terminal_argument;
	nsi_context_owner_exit_fn owner_exit;
	int terminated;
} binding;

nsi_context_status_t nsi_context_bind(const nsi_context_if_t *interface, uint64_t owner,
				      nsi_context_t owner_context, size_t native_stack_size)
{
	nsi_context_status_t status;

	if (interface == NULL) {
		interface = nsi_context_default_if();
	}
	status = nsi_context_check_abi(interface);
	if (status != NSI_CONTEXT_OK) {
		return status;
	}
	if (binding.interface != NULL || owner == 0) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	if (native_stack_size == 0) {
		native_stack_size = NSI_CONTEXT_DEFAULT_STACK_SIZE;
	}
	if (owner_context == 0) {
		status = interface->attach_root(interface->user, &owner_context);
		if (status != NSI_CONTEXT_OK) {
			return status;
		}
	}
	binding.interface = interface;
	binding.owner = owner;
	binding.stack_size = native_stack_size;
	binding.owner_context = owner_context;
	binding.active_context = owner_context;
	return NSI_CONTEXT_OK;
}

nsi_context_status_t nsi_context_bind_default(void)
{
	if (binding.interface != NULL) {
		return NSI_CONTEXT_OK;
	}
	return nsi_context_bind(NULL, NSI_CONTEXT_DEFAULT_OWNER, 0,
				NSI_CONTEXT_DEFAULT_STACK_SIZE);
}

nsi_context_status_t nsi_context_unbind(void)
{
	if (binding.interface != NULL && binding.active_context != binding.owner_context) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	memset(&binding, 0, sizeof(binding));
	return NSI_CONTEXT_OK;
}

nsi_context_status_t nsi_context_create_bound(nsi_context_role_t role,
					      nsi_context_entry_fn entry, void *argument,
					      nsi_context_t *context,
					      nsi_context_stack_t *stack)
{
	nsi_context_status_t status;

	if (binding.interface == NULL || context == NULL || stack == NULL) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	*context = 0;
	memset(stack, 0, sizeof(*stack));

	status = binding.interface->allocate_stack(binding.interface->user, binding.stack_size,
						   stack);
	if (status != NSI_CONTEXT_OK) {
		return status;
	}
	status = binding.interface->create(binding.interface->user, binding.owner, role, *stack,
					   entry, argument, context);
	if (status != NSI_CONTEXT_OK) {
		(void)binding.interface->release_stack(binding.interface->user, *stack);
		memset(stack, 0, sizeof(*stack));
	}
	return status;
}

nsi_context_status_t nsi_context_switch_bound(nsi_context_t destination)
{
	nsi_context_t source;
	nsi_context_status_t status;

	if (binding.interface == NULL || destination == 0) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	source = binding.active_context;
	binding.active_context = destination;
	status = binding.interface->switch_to(binding.interface->user, source, destination);
	binding.active_context = source;

	/*
	 * Control came back to the owner because the CPU terminated, not
	 * because it halted. The embedder leaves through its own exit point
	 * before any more HW model code runs, and must not return here.
	 */
	if (status == NSI_CONTEXT_OK && binding.terminated &&
	    source == binding.owner_context && binding.owner_exit != NULL) {
		binding.owner_exit();
		__builtin_trap();
	}
	return status;
}

nsi_context_status_t nsi_context_abandon_bound(nsi_context_t destination)
{
	nsi_context_t source;
	nsi_context_status_t status;

	if (binding.interface == NULL || destination == 0) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	source = binding.active_context;
	binding.active_context = destination;
	status = binding.interface->abandon_to(binding.interface->user, source, destination);
	binding.active_context = source;
	return status;
}

nsi_context_status_t nsi_context_destroy_bound(nsi_context_t *context,
					       nsi_context_stack_t *stack)
{
	nsi_context_status_t status;

	if (binding.interface == NULL || context == NULL || stack == NULL) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	if (*context != 0) {
		status = binding.interface->destroy(binding.interface->user, *context);
		if (status != NSI_CONTEXT_OK) {
			return status;
		}
		*context = 0;
	}
	if (stack->usable_begin != NULL) {
		status = binding.interface->release_stack(binding.interface->user, *stack);
		if (status != NSI_CONTEXT_OK) {
			return status;
		}
		memset(stack, 0, sizeof(*stack));
	}
	return NSI_CONTEXT_OK;
}

nsi_context_status_t nsi_context_set_terminal_handler(nsi_context_terminal_fn handler,
						      void *argument)
{
	if (binding.interface == NULL) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	binding.terminal_handler = handler;
	binding.terminal_argument = argument;
	return NSI_CONTEXT_OK;
}

nsi_context_status_t nsi_context_set_owner_exit(nsi_context_owner_exit_fn handler)
{
	if (binding.interface == NULL) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	binding.owner_exit = handler;
	return NSI_CONTEXT_OK;
}

nsi_context_status_t nsi_context_terminate_bound(void)
{
	if (binding.interface == NULL || binding.active_context == binding.owner_context) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	binding.terminated = 1;
	if (binding.terminal_handler != NULL) {
		binding.terminal_handler(binding.terminal_argument);
	}
	return nsi_context_abandon_bound(binding.owner_context);
}

nsi_context_t nsi_context_active(void)
{
	return binding.active_context;
}

nsi_context_t nsi_context_owner(void)
{
	return binding.owner_context;
}

int nsi_context_is_bound(void)
{
	return binding.interface != NULL;
}
