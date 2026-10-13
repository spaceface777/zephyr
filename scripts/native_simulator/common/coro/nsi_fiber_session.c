/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <errno.h>
#include <fenv.h>
#include <stdio.h>
#include <xmmintrin.h>
#include "nsi_fiber_session.h"
#include "nsi_utils.h"

/* Compile this provider once in the host, never once per localized domain. */
static _Thread_local struct nsi_fiber_session *registered_session;

struct host_state {
	int err;
	fenv_t fenv;
	uint32_t mxcsr;
};

static NSI_FUNC_NORETURN void session_error(const char *reason)
{
	/* Ownership violations cannot safely enter another domain's exit path. */
	fprintf(stderr, "nsi_fiber_session: %s\n", reason);
	ExitProcess(1U);
}

static void save_host(struct host_state *state)
{
	state->err = errno;
	state->mxcsr = _mm_getcsr();
	if (fegetenv(&state->fenv) != 0) {
		session_error("cannot save host FP state");
	}
}

static void restore_host(struct host_state *state)
{
	if (fesetenv(&state->fenv) != 0) {
		session_error("cannot restore host FP state");
	}
	_mm_setcsr(state->mxcsr);
	errno = state->err;
}

void nsi_fiber_session_check(struct nsi_fiber_session *session)
{
	if (session == NULL || registered_session != session || !session->opened ||
	    GetCurrentThreadId() != session->owner || IsThreadAFiber() == FALSE) {
		session_error("session used outside its owner thread");
	}
}

static void check_root(struct nsi_fiber_session *session)
{
	nsi_fiber_session_check(session);
	if (GetCurrentFiber() != session->root) {
		session_error("operation requires the session root");
	}
	if (!session->owned && GetFiberData() != session->data) {
		session_error("borrowed root data changed");
	}
}

void nsi_fiber_session_open(struct nsi_fiber_session *session)
{
	struct host_state state;

	if (session == NULL || session->opened || registered_session != NULL) {
		session_error("a session is already open on this thread");
	}
	save_host(&state);
	*session = (struct nsi_fiber_session){.owner = GetCurrentThreadId(),
		.owned = IsThreadAFiber() == FALSE};
	session->root = session->owned ? ConvertThreadToFiberEx(NULL, FIBER_FLAG_FLOAT_SWITCH) :
					GetCurrentFiber();
	if (session->root == NULL) {
		session_error("cannot convert host thread");
	}
	session->data = GetFiberData();
	session->opened = true;
	registered_session = session;
	restore_host(&state);
}

void nsi_fiber_session_close(struct nsi_fiber_session *session)
{
	struct host_state state;

	check_root(session);
	if (session->domains != 0U || session->active != NULL) {
		session_error("cannot close a referenced or active session");
	}
	save_host(&state);
	if (session->owned && ConvertFiberToThread() == FALSE) {
		session_error("cannot release owned root conversion");
	}
	registered_session = NULL;
	*session = (struct nsi_fiber_session){0};
	restore_host(&state);
}

void nsi_fiber_session_join(struct nsi_fiber_session *session)
{
	check_root(session);
	if (session->active != NULL || session->domains == SIZE_MAX) {
		session_error("cannot attach during an active grant");
	}
	session->domains++;
}

void nsi_fiber_session_drop(struct nsi_fiber_session *session)
{
	check_root(session);
	if (session->active != NULL || session->domains == 0U) {
		session_error("cannot detach an active or unreferenced domain");
	}
	session->domains--;
}

void nsi_fiber_session_enter(struct nsi_fiber_session *session, void *domain)
{
	check_root(session);
	if (domain == NULL || session->active != NULL || session->domains == 0U) {
		session_error("nested or foreign-domain grant");
	}
	session->active = domain;
}

void nsi_fiber_session_leave(struct nsi_fiber_session *session, void *domain)
{
	check_root(session);
	if (domain == NULL || session->active != domain) {
		session_error("grant released by a different domain");
	}
	session->active = NULL;
}
