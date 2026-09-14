/*
 * Copyright (c) 2026 Quercus
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "nsi_context_internal.h"

struct nsi_context_binding {
	const nsi_context_if_t *interface;
	uint64_t owner;
	size_t stack_size;
	nsi_context_t *owner_context;
	nsi_context_t *active_context;
	nsi_context_terminal_fn terminal_handler;
	void *terminal_argument;
};

/* This object is duplicated with each embedded runner instance. */
static struct nsi_context_binding binding;

nsi_context_status_t nsi_context_bind(const nsi_context_if_t *interface, uint64_t owner,
				      nsi_context_t *owner_context, size_t native_stack_size)
{
	nsi_context_status_t status = nsi_context_check_abi(interface);

	if (status != NSI_CONTEXT_OK) {
		return status;
	}
	if (binding.interface != NULL || interface->create == NULL ||
	    interface->switch_to == NULL || interface->abandon_to == NULL ||
	    interface->destroy == NULL || interface->stack_bounds == NULL ||
	    interface->allocate_stack == NULL || interface->release_stack == NULL) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	if (owner_context == NULL) {
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

nsi_context_status_t nsi_context_unbind(void)
{
	if (binding.interface == NULL) {
		return NSI_CONTEXT_OK;
	}
	if (binding.active_context != binding.owner_context) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	memset(&binding, 0, sizeof(binding));
	return NSI_CONTEXT_OK;
}

nsi_context_status_t nsi_context_create_bound(nsi_context_role_t role, nsi_context_entry_fn entry,
					      void *argument, nsi_context_t **context,
					      nsi_context_stack_t *stack)
{
	nsi_context_status_t status;

	if (binding.interface == NULL || context == NULL || stack == NULL) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	*context = NULL;
	*stack = (nsi_context_stack_t){0};
	status = binding.interface->allocate_stack(binding.interface->user, binding.stack_size,
						   stack);
	if (status != NSI_CONTEXT_OK) {
		return status;
	}
	status = binding.interface->create(binding.interface->user, binding.owner, role, *stack,
					   entry, argument, context);
	if (status != NSI_CONTEXT_OK) {
		(void)binding.interface->release_stack(binding.interface->user, *stack);
		*stack = (nsi_context_stack_t){0};
	}
	return status;
}

nsi_context_status_t nsi_context_switch_bound(nsi_context_t *destination)
{
	nsi_context_t *source;
	nsi_context_status_t status;

	if (binding.interface == NULL || destination == NULL) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	source = binding.active_context;
	binding.active_context = destination;
	status = binding.interface->switch_to(binding.interface->user, source, destination);
	if (status == NSI_CONTEXT_OK) {
		binding.active_context = source;
	} else {
		binding.active_context = source;
	}
	return status;
}

nsi_context_status_t nsi_context_abandon_bound(nsi_context_t *destination)
{
	nsi_context_t *source;
	nsi_context_status_t status;

	if (binding.interface == NULL || destination == NULL) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	source = binding.active_context;
	binding.active_context = destination;
	status = binding.interface->abandon_to(binding.interface->user, source, destination);
	binding.active_context = source;
	return status;
}

nsi_context_status_t nsi_context_destroy_bound(nsi_context_t **context, nsi_context_stack_t *stack)
{
	nsi_context_status_t status;

	if (binding.interface == NULL || context == NULL || stack == NULL) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	if (*context != NULL) {
		status = binding.interface->destroy(binding.interface->user, *context);
		if (status != NSI_CONTEXT_OK) {
			return status;
		}
		*context = NULL;
	}
	if (stack->usable_begin != NULL) {
		status = binding.interface->release_stack(binding.interface->user, *stack);
		if (status != NSI_CONTEXT_OK) {
			return status;
		}
		*stack = (nsi_context_stack_t){0};
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

nsi_context_status_t nsi_context_terminate_bound(void)
{
	if (binding.interface == NULL || binding.active_context == binding.owner_context) {
		return NSI_CONTEXT_WRONG_STATE;
	}
	if (binding.terminal_handler != NULL) {
		binding.terminal_handler(binding.terminal_argument);
	}
	return nsi_context_abandon_bound(binding.owner_context);
}

nsi_context_t *nsi_context_active(void)
{
	return binding.active_context;
}

nsi_context_t *nsi_context_owner(void)
{
	return binding.owner_context;
}

int nsi_context_is_bound(void)
{
	return binding.interface != NULL;
}
