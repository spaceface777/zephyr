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
 * Minimal stackful coroutine switch (see nsi_coro.S).
 *
 * A switch saves the callee-saved registers of the running code on its own
 * stack, moves to the target stack, and restores the target's registers from
 * it. Nothing else is saved, queued or scheduled.
 *
 * A continuation is the saved stack pointer of a suspended coroutine. It is
 * valid for exactly one resume.
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

#ifdef __cplusplus
}
#endif

#endif /* NSI_COMMON_CORO_NSI_CORO_H */
