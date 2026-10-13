/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/* GetCurrentThreadStackLimits() needs Windows 8. */
#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <errno.h>
#include <fenv.h>
#include <intrin.h>
#include <xmmintrin.h>
#include "host.h"

#define RAISE_CODE 0xE0000001U

typedef EXCEPTION_DISPOSITION seh_handler_t(struct _EXCEPTION_RECORD *record, void *frame,
					  struct _CONTEXT *context, void *dispatcher);

struct seh_record {
	struct seh_record *next;
	seh_handler_t *handler;
};

static volatile LONG handled;

static uint16_t control(void)
{
	uint16_t value;

	__asm__ volatile("fnstcw %0" : "=m"(value));
	return value;
}

static uint16_t status(void)
{
	uint16_t value;

	__asm__ volatile("fnstsw %0" : "=m"(value));
	return value;
}

static void capture(struct host_state *s)
{
	s->err = errno;
	s->cw = control();
	s->sw = status() & 0x3fU;
	s->mxcsr = _mm_getcsr();
	s->owner = GetCurrentThreadId();
	s->errno_addr = (uintptr_t)&errno;
}

int host_set_state(struct host_state *s, uint32_t id)
{
	if (feclearexcept(FE_ALL_EXCEPT) != 0 ||
	    fesetround(id == 1U ? FE_DOWNWARD : FE_UPWARD) != 0 ||
	    feraiseexcept(id == 1U ? FE_INVALID : FE_DIVBYZERO) != 0) {
		return -1;
	}
	_mm_setcsr(0x1f80U | (id == 1U ? 0x2001U : 0x4004U));
	errno = (int)(100U + id);
	capture(s);
	return 0;
}

int host_check_state(const struct host_state *s)
{
	struct host_state actual;

	capture(&actual);
	return (actual.err == s->err && actual.cw == s->cw && actual.sw == s->sw &&
		actual.mxcsr == s->mxcsr && actual.owner == s->owner)
		       ? 0
		       : -1;
}

int host_bounds(uintptr_t addr, size_t size, uint32_t sentinel)
{
	MEMORY_BASIC_INFORMATION m;

	if (addr == 0U || size < 4096U || addr + size < addr) {
		return -1;
	}
	/* The whole published stack is committed and writable. */
	for (uintptr_t p = addr; p < addr + size; p += m.RegionSize) {
		if (VirtualQuery((void *)p, &m, sizeof(m)) != sizeof(m) || m.State != MEM_COMMIT ||
		    m.Protect != PAGE_READWRITE || m.RegionSize == 0U) {
			return -1;
		}
	}
	/* The page below it is reserved and inaccessible. */
	if (VirtualQuery((void *)(addr - 1U), &m, sizeof(m)) != sizeof(m) ||
	    m.State != MEM_RESERVE || *(volatile uint32_t *)addr != sentinel) {
		return -1;
	}
	return 0;
}

int host_current_stack(uintptr_t addr, size_t size)
{
	ULONG_PTR low;
	ULONG_PTR high;
	uintptr_t frame = (uintptr_t)&low;

	/* Windows must describe the running coroutine stack, not the thread stack. */
	GetCurrentThreadStackLimits(&low, &high);
	return (low == addr && high <= addr + size && high + 4096U > addr + size && frame >= low &&
		frame < high)
		       ? 0
		       : -1;
}

static EXCEPTION_DISPOSITION __cdecl handler(struct _EXCEPTION_RECORD *record, void *frame,
					     struct _CONTEXT *context, void *dispatcher)
{
	(void)frame;
	(void)context;
	(void)dispatcher;
	if (record->ExceptionCode == RAISE_CODE &&
	    (record->ExceptionFlags & EXCEPTION_UNWINDING) == 0U) {
		handled++;
		return ExceptionContinueExecution;
	}
	return ExceptionContinueSearch;
}

int host_raise_handled(void)
{
	volatile struct seh_record record;
	LONG before = handled;

	/* Register a frame-based handler on this stack, as SEH-using code does. */
	record.handler = handler;
	record.next = (struct seh_record *)(uintptr_t)__readfsdword(0);
	__writefsdword(0, (DWORD)(uintptr_t)&record);
	RaiseException(RAISE_CODE, 0U, 0U, NULL);
	__writefsdword(0, (DWORD)(uintptr_t)record.next);
	return handled - before == 1 ? 0 : -1;
}

int host_stack_released(uintptr_t addr)
{
	MEMORY_BASIC_INFORMATION m;

	return (VirtualQuery((void *)addr, &m, sizeof(m)) == sizeof(m) && m.State == MEM_FREE)
		       ? 0
		       : -1;
}

uint32_t host_thread_id(void)
{
	return GetCurrentThreadId();
}
