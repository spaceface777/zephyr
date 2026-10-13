/* SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <errno.h>
#include <fenv.h>
#include <stdio.h>
#include <xmmintrin.h>
#include "nsi_coro_ctx.h"
#include "state.h"
#ifdef NSI_NCE_BACKEND_COROUTINES
#include "nsi_tasks.h"
#endif
static uint16_t control(void)
{
	uint16_t v;

	__asm__ volatile("fnstcw %0" : "=m"(v));
	return v;
}
static uint16_t status(void)
{
	uint16_t v;

	__asm__ volatile("fnstsw %0" : "=m"(v));
	return v;
}
void host_capture_state(struct host_state *s)
{
	s->err = errno;
	s->cw = control();
	s->sw = status() & 0x3fU;
	s->mxcsr = _mm_getcsr();
	s->owner = GetCurrentThreadId();
	s->errno_addr = (uintptr_t)&errno;
}
int host_saved_errno(void)
{
	return nsi_coro_ctx_current()->err;
}
int host_set_state(struct host_state *s, uint32_t id)
{
	if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
	    fesetround(id == 1U ? FE_DOWNWARD : FE_UPWARD) != 0 ||
	    feraiseexcept(id == 1U ? FE_INVALID : FE_DIVBYZERO) != 0) {
		return -1;
	}
	_mm_setcsr(0x1f80U | (id == 1U ? 0x2001U : 0x4004U));
	s->cw = control();
	s->sw = status() & 0x3fU;
	s->mxcsr = _mm_getcsr();
	s->owner = GetCurrentThreadId();
	s->errno_addr = (uintptr_t)&errno;
	s->err = (int)(100U + id);
	errno = s->err;
	return 0;
}
int host_check_state(const struct host_state *s)
{
	return errno == s->err && control() == s->cw && (status() & 0x3fU) == s->sw &&
			       _mm_getcsr() == s->mxcsr && GetCurrentThreadId() == s->owner
		       ? 0
		       : -1;
}
uint32_t host_thread_id(void)
{
	return GetCurrentThreadId();
}
int host_bounds(uintptr_t addr, size_t size, uint32_t sentinel)
{
	MEMORY_BASIC_INFORMATION m;

	if (addr == 0U || size < 7U * 1024U * 1024U || addr + size < addr) {
		return -1;
	}
	for (uintptr_t p = addr; p < addr + size; p += m.RegionSize) {
		if (VirtualQuery((void *)p, &m, sizeof(m)) != sizeof(m) || m.State != MEM_COMMIT ||
		    m.Protect != PAGE_READWRITE || m.RegionSize == 0U ||
		    m.RegionSize > addr + size - p) {
			return -1;
		}
	}
	if (VirtualQuery((void *)(addr - 1U), &m, sizeof(m)) != sizeof(m) ||
	    m.Protect != (PAGE_READWRITE | PAGE_GUARD) || *(uint32_t *)addr != sentinel) {
		return -1;
	}
	return 0;
}
static DWORD WINAPI foreign_entry(void *arg)
{
	(void)arg;
	(void)nsi_coro_ctx_current();
	return 0U;
}
void host_wrong_owner(void)
{
	HANDLE thread = CreateThread(NULL, 0U, foreign_entry, NULL, 0U, NULL);

	if (thread == NULL || WaitForSingleObject(thread, 10000U) != WAIT_OBJECT_0) {
		ExitProcess(97U);
	}
	CloseHandle(thread);
	ExitProcess(98U);
}
static struct host_cleanup *cleanup_active;
static VOID WINAPI cleanup_callback(void *value)
{
	/* Do not call Zephyr assertions or schedule from a native deletion callback. */
	struct host_cleanup *cleanup = cleanup_active;

	if (cleanup == NULL) {
		ExitProcess(96U);
	}
	if (value != cleanup || cleanup->registered != 1U || cleanup->callbacks != 0U ||
	    GetCurrentThreadId() != cleanup->owner) {
		cleanup->errors++;
	}
	cleanup->callbacks++;
	if (feclearexcept(FE_ALL_EXCEPT) != 0 || fesetround(FE_UPWARD) != 0 ||
	    feraiseexcept(FE_DIVBYZERO) != 0) {
		cleanup->errors++;
	}
	_mm_setcsr(0x5f84U);
	errno = 777;
}
int host_cleanup_open(struct host_cleanup *cleanup)
{
	if (cleanup_active != NULL) {
		return -1;
	}
	*cleanup = (struct host_cleanup){.owner = GetCurrentThreadId()};
	cleanup->slot = FlsAlloc(cleanup_callback);
	if (cleanup->slot == FLS_OUT_OF_INDEXES) {
		return -1;
	}
	cleanup_active = cleanup;
	return FlsGetValue(cleanup->slot) == NULL ? 0 : -1;
}
int host_cleanup_register(struct host_cleanup *cleanup)
{
	if (cleanup != cleanup_active || cleanup->registered != 0U ||
	    GetCurrentThreadId() != cleanup->owner ||
	    FlsSetValue(cleanup->slot, cleanup) == FALSE) {
		return -1;
	}
	cleanup->registered++;
	return 0;
}
int host_cleanup_close(struct host_cleanup *cleanup)
{
	/* The registered child must already be deleted. The caller owns no slot value. */
	if (cleanup != cleanup_active || cleanup->registered != 1U || cleanup->callbacks != 1U ||
	    cleanup->errors != 0U || FlsGetValue(cleanup->slot) != NULL) {
		return -1;
	}
	if (FlsFree(cleanup->slot) == FALSE) {
		return -1;
	}
	cleanup_active = NULL;
	return cleanup->callbacks == 1U && cleanup->errors == 0U ? 0 : -1;
}
void host_borrow_root(void)
{
	if (IsThreadAFiber() != FALSE || ConvertThreadToFiberEx((void *)0x1234U, 0U) == NULL) {
		ExitProcess(99U);
	}
	printf("BORROWED ROOT attached by fixture before backend creation\n");
}
#ifdef NSI_NCE_BACKEND_COROUTINES
#ifdef TEST_FIBER_BORROWED_ROOT
NSI_TASK(host_borrow_root, PRE_BOOT_1, 0);
#endif
static void check_root_cleanup(void)
{
	if (!nsi_fiber_domain_is_root() || nsi_fiber_domain_contexts() != 0U ||
	    cleanup_active != NULL || IsThreadAFiber() == FALSE) {
		ExitProcess(95U);
	}
#ifdef TEST_FIBER_BORROWED_ROOT
	if (GetFiberData() != (void *)0x1234U) {
		ExitProcess(94U);
	}
#endif
	printf("FIBERS ROOT EXIT: zero contexts, callbacks complete, root still attached\n");
}
NSI_TASK(check_root_cleanup, ON_EXIT_POST, 99);
#endif
