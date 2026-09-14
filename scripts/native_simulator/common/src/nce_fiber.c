/*
 * Copyright (c) 2026 Quercus
 * SPDX-License-Identifier: Apache-2.0
 */

/* Single-host-thread Native Simulator NCE backend. */

#include <stdbool.h>
#include <stdlib.h>

#include "nce_if.h"
#include "nsi_context_internal.h"

struct nce_status {
	nsi_context_t *owner_context;
	nsi_context_t *bootstrap_context;
	nsi_context_stack_t bootstrap_stack;
	nsi_context_t *firmware_context;
	void (*start_routine)(void);
	bool cpu_halted;
	bool booted;
	bool terminal;
};

/* One private NCE exists per one-CPU materialized runner instance. */
static struct nce_status instance_status;

static void terminal_handler(void *argument)
{
	struct nce_status *status = argument;

	status->terminal = true;
	status->cpu_halted = true;
	status->firmware_context = NULL;
}

static void bootstrap_entry(void *argument)
{
	struct nce_status *status = argument;

	status->start_routine();
	(void)nsi_context_terminate_bound();
}

void *nce_init(void)
{
	struct nce_status *status = &instance_status;

	if (!nsi_context_is_bound() || (status->owner_context != NULL && !status->terminal)) {
		return NULL;
	}
	*status = (struct nce_status){0};
	status->owner_context = nsi_context_owner();
	status->cpu_halted = true;
	if (nsi_context_set_terminal_handler(terminal_handler, status) != NSI_CONTEXT_OK) {
		status->owner_context = NULL;
		return NULL;
	}
	return status;
}

void nce_boot_cpu(void *this_arg, void (*start_routine)(void))
{
	struct nce_status *status = this_arg;

	if (status == NULL || start_routine == NULL || status->booted || status->terminal ||
	    nsi_context_active() != status->owner_context) {
		return;
	}
	status->start_routine = start_routine;
	if (nsi_context_create_bound(NSI_CONTEXT_ZEPHYR_BOOTSTRAP, bootstrap_entry, status,
				     &status->bootstrap_context,
				     &status->bootstrap_stack) != NSI_CONTEXT_OK) {
		status->terminal = true;
		return;
	}
	status->booted = true;
	status->firmware_context = status->bootstrap_context;
	nce_wake_cpu(status);
}

void nce_halt_cpu(void *this_arg)
{
	struct nce_status *status = this_arg;

	if (status == NULL || status->terminal || status->cpu_halted ||
	    nsi_context_active() == status->owner_context) {
		return;
	}
	status->firmware_context = nsi_context_active();
	status->cpu_halted = true;
	(void)nsi_context_switch_bound(status->owner_context);
}

void nce_wake_cpu(void *this_arg)
{
	struct nce_status *status = this_arg;

	if (status == NULL || status->terminal || !status->cpu_halted ||
	    status->firmware_context == NULL || nsi_context_active() != status->owner_context) {
		return;
	}
	status->cpu_halted = false;
	(void)nsi_context_switch_bound(status->firmware_context);
	/* A normal return is possible only after halt or terminal transfer. */
	if (!status->terminal) {
		status->cpu_halted = true;
	}
}

void nce_terminate(void *this_arg)
{
	struct nce_status *status = this_arg;

	if (status == NULL) {
		return;
	}
	if (nsi_context_active() != status->owner_context) {
		status->terminal = true;
		status->cpu_halted = true;
		status->firmware_context = NULL;
		(void)nsi_context_terminate_bound();
		return;
	}
	status->terminal = true;
	status->cpu_halted = true;
	status->firmware_context = NULL;
	(void)nsi_context_set_terminal_handler(NULL, NULL);
	(void)nsi_context_destroy_bound(&status->bootstrap_context, &status->bootstrap_stack);
	/* The control object is instance-owned static storage, not an allocation. */
}

int nce_is_cpu_running(void *this_arg)
{
	struct nce_status *status = this_arg;

	return status != NULL && !status->terminal && !status->cpu_halted;
}
