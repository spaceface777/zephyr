/*
 * Copyright (c) 2026 Quercus
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NSI_CONTEXT_INTERNAL_H
#define NSI_CONTEXT_INTERNAL_H

#include "nsi_context_if.h"

#ifdef __cplusplus
extern "C" {
#endif

nsi_context_status_t nsi_context_create_bound(nsi_context_role_t role, nsi_context_entry_fn entry,
					      void *argument, nsi_context_t **context,
					      nsi_context_stack_t *stack);
nsi_context_status_t nsi_context_switch_bound(nsi_context_t *destination);
nsi_context_status_t nsi_context_abandon_bound(nsi_context_t *destination);
nsi_context_status_t nsi_context_destroy_bound(nsi_context_t **context, nsi_context_stack_t *stack);
typedef void (*nsi_context_terminal_fn)(void *argument);
nsi_context_status_t nsi_context_set_terminal_handler(nsi_context_terminal_fn handler,
						      void *argument);
nsi_context_status_t nsi_context_terminate_bound(void);
nsi_context_t *nsi_context_active(void);
nsi_context_t *nsi_context_owner(void);
int nsi_context_is_bound(void);

#ifdef __cplusplus
}
#endif

#endif /* NSI_CONTEXT_INTERNAL_H */
