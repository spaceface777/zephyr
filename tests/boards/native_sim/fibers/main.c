/* SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/ztest.h>
#include "state.h"

#ifdef CONFIG_NATIVE_SIMULATOR_COROUTINE_IMPLEMENTATION_WINDOWS_FIBERS

#define CYCLES   32U
#define ROUNDS   256U
#define SENTINEL 0xf0f0f0f0U
static struct k_thread workers[2];
K_THREAD_STACK_ARRAY_DEFINE(stacks, 2, 2048);
static struct k_sem turns[2];
static struct host_state states[2];
static uint32_t counts[2];
static uint32_t cpu_thread;

#if defined(CONFIG_TEST_FIBER_BORROWED_ROOT) &&                                                    \
	!defined(CONFIG_NATIVE_SIMULATOR_NCE_BACKEND_COROUTINES)
static int borrow_root(void)
{
	host_borrow_root();
	return 0;
}
SYS_INIT(borrow_root, PRE_KERNEL_1, 0);
#endif

static void worker(void *arg, void *unused1, void *unused2)
{
	uint32_t i = (uint32_t)(uintptr_t)arg;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	zassert_equal(host_thread_id(), cpu_thread);
	zassert_ok(host_bounds(workers[i].stack_info.start, workers[i].stack_info.size, SENTINEL));
	zassert_ok(host_set_state(&states[i], i + 1U));
	for (uint32_t round = 0U; round < ROUNDS; round++) {
		zassert_ok(k_sem_take(&turns[i], K_FOREVER));
		zassert_ok(host_check_state(&states[i]), "FP/errno state changed at round %u",
			   round);
		counts[i]++;
		k_sem_give(&turns[1U - i]);
	}
	zassert_ok(host_bounds(workers[i].stack_info.start, workers[i].stack_info.size, SENTINEL));
	/* Returning invokes Zephyr self-abort, then deferred fiber deletion. */
}

ZTEST(fibers_state, test_state_bounds_and_lifecycle)
{
	cpu_thread = host_thread_id();
	if (IS_ENABLED(CONFIG_TEST_FIBER_WRONG_OWNER)) {
		host_wrong_owner();
		zassert_unreachable("foreign owner must fail");
	}
	for (uint32_t cycle = 0U; cycle < CYCLES; cycle++) {
		for (uint32_t i = 0U; i < 2U; i++) {
			counts[i] = 0U;
			k_sem_init(&turns[i], i == 0U ? 1U : 0U, 1U);
			k_thread_create(&workers[i], stacks[i], K_THREAD_STACK_SIZEOF(stacks[i]),
					worker, (void *)(uintptr_t)i, NULL, NULL, K_PRIO_COOP(1),
					0U, K_FOREVER);
			/* Real arch_new_thread publishes bounds and sentinel before first entry. */
			zassert_ok(host_bounds(workers[i].stack_info.start,
					       workers[i].stack_info.size, SENTINEL));
		}
		k_thread_start(&workers[0]);
		k_thread_start(&workers[1]);
		for (uint32_t i = 0U; i < 2U; i++) {
			zassert_ok(k_thread_join(&workers[i], K_SECONDS(5)));
			zassert_equal(counts[i], ROUNDS);
			zassert_equal(states[i].owner, cpu_thread);
		}
		zassert_not_equal(states[0].errno_addr, states[1].errno_addr);
	}
	printk("FIBERS LIFECYCLE cycles=%u rounds_each=%u real sentinel/guard/state checks "
	       "passed\n",
	       CYCLES, ROUNDS);
}
static struct k_thread cleanup_child;
K_THREAD_STACK_DEFINE(cleanup_stack, 2048);
K_SEM_DEFINE(cleanup_ready, 0, 1);
K_SEM_DEFINE(cleanup_wait, 0, 1);

static void cleanup_entry(void *arg, void *direct, void *unused)
{
	struct host_cleanup *cleanup = arg;
	struct host_state child_state;

	ARG_UNUSED(unused);
	zassert_ok(host_cleanup_register(cleanup));
	zassert_ok(host_set_state(&child_state, 2U));
	k_sem_give(&cleanup_ready);
	if (direct != NULL) {
		zassert_ok(k_sem_take(&cleanup_wait, K_FOREVER));
		zassert_unreachable("the waiting child must be aborted");
	}
	/* Return through Zephyr self-abort and the shared deferred-deletion path. */
}

static void check_cleanup(bool direct)
{
	struct host_cleanup cleanup;
	struct host_state stale, live, actual;
	int saved_errno, state_result, close_result;

	zassert_ok(host_cleanup_open(&cleanup));
	k_sem_reset(&cleanup_ready);
	k_sem_reset(&cleanup_wait);
	k_thread_create(&cleanup_child, cleanup_stack, K_THREAD_STACK_SIZEOF(cleanup_stack),
			cleanup_entry, &cleanup, direct ? &cleanup : NULL, NULL, K_PRIO_COOP(1), 0U,
			K_FOREVER);
	zassert_ok(host_bounds(cleanup_child.stack_info.start, cleanup_child.stack_info.size,
			       SENTINEL));
	if (direct) {
		zassert_ok(host_set_state(&stale, 2U));
		k_thread_start(&cleanup_child);
		zassert_ok(k_sem_take(&cleanup_ready, K_SECONDS(5)));
		/* The saved switch state predates this live state, with no intervening yield. */
		saved_errno = host_saved_errno();
		zassert_equal(saved_errno, stale.err);
		zassert_ok(host_set_state(&live, 1U));
		k_thread_abort(&cleanup_child);
	} else {
		zassert_ok(host_set_state(&live, 1U));
		k_thread_start(&cleanup_child);
		zassert_ok(k_thread_join(&cleanup_child, K_SECONDS(5)));
		saved_errno = live.err;
	}
	/* Capture before join, diagnostics, or slot release can alter the host state. */
	state_result = host_check_state(&live);
	host_capture_state(&actual);
	zassert_ok(k_thread_join(&cleanup_child, K_NO_WAIT));
	close_result = host_cleanup_close(&cleanup);
	printk("FLS CLEANUP direct=%d callbacks=%u errors=%u saved_errno=%d "
	       "live_errno=%d actual_errno=%d cw=%x/%x sw=%x/%x mxcsr=%x/%x\n",
	       direct, cleanup.callbacks, cleanup.errors, saved_errno, live.err, actual.err,
	       actual.cw, live.cw, actual.sw, live.sw, actual.mxcsr, live.mxcsr);
	zassert_ok(close_result, "the deleted child must release its FLS slot exactly once");
	zassert_ok(state_result, "FLS cleanup changed the caller's live errno/FP state");
}

ZTEST(fibers_state, test_cleanup_deferred_state)
{
	check_cleanup(false);
}

ZTEST(fibers_state, test_cleanup_direct_live_state)
{
	check_cleanup(true);
}

ZTEST_SUITE(fibers_state, NULL, NULL, NULL, NULL, NULL);
#else
/* The metadata filter excludes configurations without the experimental backend. */
void test_main(void)
{
}
#endif
