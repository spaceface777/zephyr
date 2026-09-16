/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NSI_COMMON_SRC_INCL_NCT_CORO_H
#define NSI_COMMON_SRC_INCL_NCT_CORO_H

#include <stddef.h>

#include "nsi_context_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read only accessors into the coroutine NCT backend, meant for debuggers.
 *
 * With this backend the embedded threads are coroutines rather than host
 * threads, so a debugger cannot find them by asking the OS. These report what
 * it needs to list them and to unwind a suspended one. None of them changes
 * NCT or embedded OS state.
 *
 * `this_arg` is the value nct_init() returned. Thread indexes are the ones
 * nct_new_thread() returned, and are never reused.
 */

/* Return the NCT instance for a read-only embedded debugger adapter. */
void *nct_coro_debug_instance(void);

/* Number of threads ever created, which is also one past the highest index. */
int nct_coro_debug_thread_count(void *this_arg);

/* Index of the thread which is running, or -1 if none is. */
int nct_coro_debug_current_thread(void *this_arg);

/* Whether `index` still names a live thread. */
int nct_coro_debug_thread_present(void *this_arg, int index);

/* The thread's name, or an empty string if it has none. Never NULL. */
const char *nct_coro_debug_thread_name(void *this_arg, int index);

/*
 * The thread's state: 0 unused, 1 ready, 2 aborting, 3 aborted, 4 finished.
 */
int nct_coro_debug_thread_state(void *this_arg, int index);

/* The context the thread runs on, or 0 if there is no such thread. */
nsi_context_t nct_coro_debug_thread_context(void *this_arg, int index);

/* Bounds of the thread's execution stack. */
void *nct_coro_debug_thread_stack_begin(void *this_arg, int index);
size_t nct_coro_debug_thread_stack_size(void *this_arg, int index);

#ifdef __cplusplus
}
#endif

#endif /* NSI_COMMON_SRC_INCL_NCT_CORO_H */
