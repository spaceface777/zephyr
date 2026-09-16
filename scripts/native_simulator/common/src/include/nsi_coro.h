/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NSI_COMMON_SRC_INCL_NSI_CORO_H
#define NSI_COMMON_SRC_INCL_NSI_CORO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal stackful coroutine switch primitive.
 *
 * A coroutine owns a stack and a saved continuation. Switching to one saves the
 * callee-saved registers and the return address of the caller on the caller's
 * own stack, moves the stack pointer, and restores the same set from the
 * target. Nothing is scheduled, queued, or preempted: control goes exactly
 * where the caller says, and comes back only when somebody switches back.
 *
 * A switch carries one value: the caller receives the continuation of whoever
 * switched back to it. That is the one thing which cannot be passed any other
 * way, because it only comes into existence during the switch itself.
 * Everything else which has to cross a switch, including which coroutine did
 * the switching, errno, the floating point environment, and the sanitizer
 * bookkeeping, belongs to the layer above and is handled in C.
 *
 * Returning a single pointer is deliberate. Every ABI this supports returns one
 * in a register, which keeps each port to a save, a stack pointer move, and a
 * restore. A second return value would be returned in memory through a hidden
 * pointer on 32 bit x86, which would put a store through a pointer owned by the
 * coroutine being resumed in the middle of the switch, and would oblige every
 * saved frame to carry that pointer.
 *
 * Only the coroutine which is running may switch away, and a coroutine may
 * not switch to itself. A coroutine must never return from its entry function;
 * it either switches away for the last time or the process ends.
 */

/* Opaque continuation. NULL is never a valid coroutine. */
typedef void *nsi_coro_t;

/*
 * Entry function of a coroutine.
 *
 * `from` is the continuation of the coroutine which performed the first switch
 * into this one. It is the only way to get back there, and it becomes stale as
 * soon as that coroutine is resumed by anybody. The entry function must not
 * return.
 */
typedef void (*nsi_coro_entry_f)(nsi_coro_t from);

/**
 * Prepare `stack_top` so that the first switch to the returned continuation
 * enters `entry`.
 *
 * `stack_top` is the high address of the usable stack, one past its last byte,
 * and must be aligned as the host ABI requires. `stack_size` is the usable size
 * below it. No memory is allocated or freed here; the caller owns the stack and
 * must keep it alive until the coroutine is finished with it.
 *
 * Returns the continuation to switch to, or NULL if the stack is unusable.
 */
nsi_coro_t nsi_coro_create(void *stack_top, size_t stack_size, nsi_coro_entry_f entry);

/**
 * Switch to `target`.
 *
 * Returns once somebody switches back, reporting the continuation of whichever
 * coroutine did so. The continuation handed to `target` replaces the one the
 * caller used, so a continuation is good for one resume only.
 */
nsi_coro_t nsi_coro_switch(nsi_coro_t target);

/*
 * Called if a coroutine entry function returns, which is a programming error.
 * It does not return. The architecture trampolines branch here.
 */
void nsi_coro_entry_returned(void);

#ifdef __cplusplus
}
#endif

#endif /* NSI_COMMON_SRC_INCL_NSI_CORO_H */
