/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NSI_COMMON_SRC_INCL_NSI_CONTEXT_INTERNAL_H
#define NSI_COMMON_SRC_INCL_NSI_CONTEXT_INTERNAL_H

#include "nsi_context_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Convenience layer over the bound context service.
 *
 * The NCT and NCE backends use these instead of reaching for the interface
 * directly, so that they do not each have to carry the bound interface
 * pointer, the owner id, and the stack size around.
 */

/*
 * Bind the runner's own context service, if nothing is bound yet.
 *
 * A program which embeds the runner binds its own service before the CPU
 * starts. A standalone executable has nobody to do that, so the backends call
 * this on their way up. It is idempotent, and it never replaces a binding an
 * embedder already made.
 */
nsi_context_status_t nsi_context_bind_default(void);

nsi_context_status_t nsi_context_create_bound(nsi_context_role_t role,
					      nsi_context_entry_fn entry, void *argument,
					      nsi_context_t *context,
					      nsi_context_stack_t *stack);

nsi_context_status_t nsi_context_switch_bound(nsi_context_t destination);

nsi_context_status_t nsi_context_abandon_bound(nsi_context_t destination);

nsi_context_status_t nsi_context_destroy_bound(nsi_context_t *context,
					       nsi_context_stack_t *stack);

/*
 * Called on the context which is being left, when a context terminates rather
 * than switching away normally.
 */
typedef void (*nsi_context_terminal_fn)(void *argument);

nsi_context_status_t nsi_context_set_terminal_handler(nsi_context_terminal_fn handler,
						      void *argument);

nsi_context_status_t nsi_context_terminate_bound(void);

/*
 * Called on the owner context, instead of returning from
 * nsi_context_switch_bound(), once a bound context has terminated. It must not
 * return: an embedding runner leaves through its operation exit point before
 * any more HW model code runs.
 */
typedef void (*nsi_context_owner_exit_fn)(void);

nsi_context_status_t nsi_context_set_owner_exit(nsi_context_owner_exit_fn handler);

nsi_context_t nsi_context_active(void);

nsi_context_t nsi_context_owner(void);

int nsi_context_is_bound(void);

#ifdef __cplusplus
}
#endif

#endif /* NSI_COMMON_SRC_INCL_NSI_CONTEXT_INTERNAL_H */
