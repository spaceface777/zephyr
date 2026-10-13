/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include "host.h"

#ifdef TEST_WINDOWS_COROUTINES

#define CYCLES      32U
#define ROUNDS      256U
#define SLEEP_EVERY 32U
#define SENTINEL    0xf0f0f0f0U

static struct k_thread workers[2];
K_THREAD_STACK_ARRAY_DEFINE(stacks, 2, 2048);
static struct k_sem turns[2];
static struct host_state states[2];
static uint32_t counts[2];
static uint32_t cpu_thread;

static void worker(void *arg, void *unused1, void *unused2)
{
	uint32_t i = (uint32_t)(uintptr_t)arg;
	struct k_thread *self = &workers[i];

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	zassert_equal(host_thread_id(), cpu_thread);
	zassert_ok(host_current_stack(self->stack_info.start, self->stack_info.size));
	zassert_ok(host_raise_handled(), "SEH dispatch failed on the coroutine stack");
	zassert_ok(host_set_state(&states[i], i + 1U));
	for (uint32_t round = 0U; round < ROUNDS; round++) {
		zassert_ok(k_sem_take(&turns[i], K_FOREVER));
		zassert_ok(host_check_state(&states[i]), "FP/errno state changed at round %u",
			   round);
		if (round % SLEEP_EVERY == 0U) {
			/* Halt the CPU, so that the hardware models run between two checks. */
			k_msleep(1);
			zassert_ok(host_check_state(&states[i]), "CPU halt changed FP/errno state");
		}
		counts[i]++;
		k_sem_give(&turns[1U - i]);
	}
	zassert_ok(host_raise_handled(), "SEH dispatch failed after context switches");
	zassert_ok(host_check_state(&states[i]));
	zassert_ok(host_current_stack(self->stack_info.start, self->stack_info.size));
	zassert_ok(host_bounds(self->stack_info.start, self->stack_info.size, SENTINEL));
	/* Returning invokes Zephyr self-abort and the deferred stack release. */
}

ZTEST(windows_coroutines, test_state_stack_and_lifecycle)
{
	uintptr_t starts[2];

	cpu_thread = host_thread_id();
	for (uint32_t cycle = 0U; cycle < CYCLES; cycle++) {
		for (uint32_t i = 0U; i < 2U; i++) {
			counts[i] = 0U;
			k_sem_init(&turns[i], i == 0U ? 1U : 0U, 1U);
			k_thread_create(&workers[i], stacks[i], K_THREAD_STACK_SIZEOF(stacks[i]),
					worker, (void *)(uintptr_t)i, NULL, NULL, K_PRIO_COOP(1),
					0U, K_FOREVER);
			/* arch_new_thread() publishes the native stack and its sentinel. */
			zassert_ok(host_bounds(workers[i].stack_info.start,
					       workers[i].stack_info.size, SENTINEL));
			starts[i] = workers[i].stack_info.start;
		}
		k_thread_start(&workers[0]);
		k_thread_start(&workers[1]);
		for (uint32_t i = 0U; i < 2U; i++) {
			zassert_ok(k_thread_join(&workers[i], K_SECONDS(5)));
			zassert_equal(counts[i], ROUNDS);
			zassert_ok(host_stack_released(starts[i]),
				   "self-aborted stack not released");
		}
		/* All coroutines share the thread's errno. The context manager keeps it apart. */
		zassert_equal(states[0].errno_addr, states[1].errno_addr);
	}
	printk("WINDOWS COROUTINES LIFECYCLE cycles=%u rounds_each=%u passed\n", CYCLES, ROUNDS);
}

static struct k_thread blocker;
K_THREAD_STACK_DEFINE(blocker_stack, 2048);
K_SEM_DEFINE(blocker_ready, 0, 1);
K_SEM_DEFINE(blocker_wait, 0, 1);

static void blocker_entry(void *unused1, void *unused2, void *unused3)
{
	struct host_state state;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	ARG_UNUSED(unused3);
	zassert_ok(host_set_state(&state, 2U));
	k_sem_give(&blocker_ready);
	(void)k_sem_take(&blocker_wait, K_FOREVER);
	zassert_unreachable("the blocked thread must be aborted");
}

ZTEST(windows_coroutines, test_abort_releases_stack)
{
	struct host_state live;

	for (uint32_t i = 0U; i < 8U; i++) {
		uintptr_t start;

		k_sem_reset(&blocker_ready);
		k_thread_create(&blocker, blocker_stack, K_THREAD_STACK_SIZEOF(blocker_stack),
				blocker_entry, NULL, NULL, NULL, K_PRIO_COOP(1), 0U, K_NO_WAIT);
		zassert_ok(k_sem_take(&blocker_ready, K_SECONDS(5)));
		/* Abort restores the Zephyr stack information, so record the native stack. */
		start = blocker.stack_info.start;
		zassert_ok(host_set_state(&live, 1U));
		k_thread_abort(&blocker);
		zassert_ok(host_check_state(&live), "abort changed the caller's FP/errno state");
		zassert_ok(host_stack_released(start), "aborted stack not released");
	}
}

ZTEST_SUITE(windows_coroutines, NULL, NULL, NULL, NULL, NULL);
#else
/* The metadata filter excludes configurations without these primitives. */
void test_main(void)
{
}
#endif
