/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Native simulator CPU start/stop emulation, stackful coroutine backend.
 *
 * This is an alternative to nce.c. The CPU boundary is a switch between the
 * context which runs the HW models and whichever embedded context last
 * halted, instead of a pair of semaphores between two host threads.
 *
 * The context which owns this boundary is the one that called nce_init(),
 * which is the context the HW models run on. Note that the context which
 * halts is not necessarily the one which was last resumed: the embedded OS
 * may switch threads before halting again.
 */

#include <stdbool.h>
#include <stdlib.h>

#include "nce_if.h"
#include "nsi_config.h"
#include "nsi_context_internal.h"
#include "nsi_main.h"
#include "nsi_tracing.h"

#if NSI_N_CPUS > 1
#error "The coroutine backend supports one embedded CPU per runner"
#endif

struct nce_status {
	nsi_context_t owner_context;
	nsi_context_t bootstrap_context;
	nsi_context_stack_t bootstrap_stack;
	nsi_context_t firmware_context;
	void (*start_routine)(void);
	bool cpu_halted;
	bool booted;
	bool terminal;
};

static void terminal_handler(void *argument)
{
	struct nce_status *status = argument;

	status->terminal = true;
	status->cpu_halted = true;
	status->firmware_context = 0;
}

static void bootstrap_entry(void *argument)
{
	struct nce_status *status = argument;

	status->start_routine();
	(void)nsi_context_terminate_bound();
}

void *nce_init(void)
{
	struct nce_status *status;

	if (nsi_context_bind_default() != NSI_CONTEXT_OK) {
		return NULL;
	}

	status = calloc(1, sizeof(*status));
	if (status == NULL) {
		return NULL;
	}
	status->owner_context = nsi_context_owner();
	status->cpu_halted = true;
	if (nsi_context_set_terminal_handler(terminal_handler, status) != NSI_CONTEXT_OK) {
		free(status);
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
	/* Whichever embedded context is running is the one which resumes next. */
	status->firmware_context = nsi_context_active();
	status->cpu_halted = true;
	(void)nsi_context_switch_bound(status->owner_context);
}

void nce_wake_cpu(void *this_arg)
{
	struct nce_status *status = this_arg;

	if (status == NULL || status->terminal || !status->cpu_halted ||
	    status->firmware_context == 0 ||
	    nsi_context_active() != status->owner_context) {
		return;
	}
	status->cpu_halted = false;
	(void)nsi_context_switch_bound(status->firmware_context);
	/* A normal return is possible only after a halt or a terminal transfer. */
	if (status->terminal) {
		/*
		 * The embedded side ended the program and handed control back
		 * here to finish it, as it does with the host thread backend.
		 * Without this the HW models would keep running with no CPU to
		 * serve, and the process would never end.
		 *
		 * The code the embedded side passed is not lost by asking for
		 * zero here: nsi_exit_inner() keeps the highest code it has
		 * been given, and it already recorded that one.
		 */
		nsi_exit(0);
	}
	status->cpu_halted = true;
}

void nce_terminate(void *this_arg)
{
	struct nce_status *status = this_arg;

	if (status == NULL) {
		return;
	}
	if (nsi_context_active() != status->owner_context) {
		/*
		 * Called from embedded code. Record the outcome and hand
		 * control back to the owner, which finishes the teardown.
		 */
		status->terminal = true;
		status->cpu_halted = true;
		status->firmware_context = 0;
		(void)nsi_context_terminate_bound();
		return;
	}
	status->terminal = true;
	status->cpu_halted = true;
	status->firmware_context = 0;
	(void)nsi_context_set_terminal_handler(NULL, NULL);
	(void)nsi_context_destroy_bound(&status->bootstrap_context, &status->bootstrap_stack);
	free(status);
}

int nce_is_cpu_running(void *this_arg)
{
	struct nce_status *status = this_arg;

	return status != NULL && !status->terminal && !status->cpu_halted;
}
