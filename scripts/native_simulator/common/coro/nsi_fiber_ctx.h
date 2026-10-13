/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NSI_COMMON_CORO_NSI_FIBER_CTX_H
#define NSI_COMMON_CORO_NSI_FIBER_CTX_H

/* Windows owns the stack and reusable handle, not a primitive continuation. */
struct nsi_coro_ctx {
	void *fiber;
	void *stack_addr;
	size_t stack_size;
	struct nsi_coro_ctx *creator;
	struct nsi_coro_ctx *domain;
	struct nsi_coro_ctx *next;
	struct nsi_coro_ctx *prev;
	int err;
	fenv_t fenv;
	uint32_t mxcsr;
	uint32_t stack_error;
	void (*entry)(void *arg);
	void *arg;
	bool ready;
	bool terminal;
};

struct nsi_fiber_session;

/**
 * Bind private domain state to an explicit shared host session before attachment.
 * @param session Open session on the host thread, or NULL for a private session.
 */
void nsi_fiber_domain_bind(struct nsi_fiber_session *session);

/** Acquire a serial root grant, attaching the domain on first use. */
void nsi_fiber_domain_enter(void);
/** Return a grant on the root, preserving the caller's restored host state. */
void nsi_fiber_domain_leave(void);
/** Detach an empty inactive domain. A private default session closes automatically. */
void nsi_fiber_domain_detach(void);
/** Report whether this domain is attached and selected on its root. */
bool nsi_fiber_domain_is_root(void);
/** Count live child contexts during an owner grant, or zero before attachment. */
size_t nsi_fiber_domain_contexts(void);
/** Hold a root maintenance grant through all exit callbacks; CPU exit transfers first. */
void nsi_fiber_domain_exit_begin(void);
/** End root cleanup and detach after all embedded and runner exit callbacks finish. */
void nsi_fiber_domain_exit_end(void);

#endif /* NSI_COMMON_CORO_NSI_FIBER_CTX_H */
