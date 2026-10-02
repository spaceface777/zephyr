/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NSI_COMMON_CORO_NSI_CORO_H
#define NSI_COMMON_CORO_NSI_CORO_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal stackful coroutine interface.
 *
 * The selected implementation may use the small architecture-specific switch
 * in nsi_coro.S or the host libc's ucontext API. Nothing here makes scheduling
 * decisions; it only transfers execution between stacks.
 *
 * A continuation is opaque to the caller and is valid for exactly one resume.
 */
typedef void *nsi_coro_t;

/*
 * Entry function of a new coroutine. <from> is the continuation of whoever
 * switched into it for the first time. It must never return.
 */
typedef void (*nsi_coro_entry_f)(nsi_coro_t from);

/*
 * Prepare a new coroutine whose stack ends (is highest) at <stack_top>, so
 * that the first switch to the returned continuation calls <entry>.
 * The caller owns the stack.
 */
nsi_coro_t nsi_coro_create(void *stack_top, nsi_coro_entry_f entry);

/*
 * Suspend the running code and resume <target>. Returns when somebody switches
 * back, with the continuation of that somebody.
 */
nsi_coro_t nsi_coro_switch(nsi_coro_t target);

/*
 * Release implementation-specific bookkeeping for a coroutine after it can no
 * longer be resumed. The caller still owns the stack itself.
 */
void nsi_coro_destroy(nsi_coro_t coro);

#ifdef __cplusplus
}
#endif

#endif /* NSI_COMMON_CORO_NSI_CORO_H */
