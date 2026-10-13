/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef NSI_COMMON_CORO_NSI_FIBER_SESSION_H
#define NSI_COMMON_CORO_NSI_FIBER_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Host-owned session. Zero-initialize before opening; do not copy an open session. */
struct nsi_fiber_session {
	void *root;
	void *data;
	void *active;
	size_t domains;
	uint32_t owner;
	bool owned;
	bool opened;
};

/**
 * Open the only session on this host thread, preserving live errno and FP state.
 * Adopt an existing fiber without changing its data, or convert the thread once.
 * The session and its provider must remain shared outside domain localization.
 * @param session Zero-initialized host storage that outlives every attached domain.
 */
void nsi_fiber_session_open(struct nsi_fiber_session *session);

/**
 * Close an empty, inactive session on its original root and owner thread.
 * Restore owned conversion only; preserve borrowed conversion and live host state.
 * Invalid ownership, active grants, and outstanding domains terminate the process.
 * @param session Open session with no domains, children, or pending exit callbacks.
 */
void nsi_fiber_session_close(struct nsi_fiber_session *session);

/* Backend-only operations. The shared TLS registry validates, but does not own, sessions. */
void nsi_fiber_session_check(struct nsi_fiber_session *session);
void nsi_fiber_session_join(struct nsi_fiber_session *session);
void nsi_fiber_session_drop(struct nsi_fiber_session *session);
void nsi_fiber_session_enter(struct nsi_fiber_session *session, void *domain);
void nsi_fiber_session_leave(struct nsi_fiber_session *session, void *domain);

#endif /* NSI_COMMON_CORO_NSI_FIBER_SESSION_H */
