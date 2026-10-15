/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include "nsi_host_trampolines.h"

K_THREAD_STACK_DEFINE(entropy_stack, 2048);

struct entropy_read {
	const struct device *dev;
	uint8_t *buffer;
	int result;
};

static void entropy_worker(void *arg, void *unused1, void *unused2)
{
	struct entropy_read *read = arg;

	ARG_UNUSED(unused1);
	ARG_UNUSED(unused2);
	read->result = entropy_get_entropy(read->dev, read->buffer, 48U);
}

static void read_on_worker(const struct device *dev, uint8_t *buffer)
{
	struct k_thread thread;
	struct entropy_read read = {.dev = dev, .buffer = buffer};
	k_tid_t tid = k_thread_create(&thread, entropy_stack, K_THREAD_STACK_SIZEOF(entropy_stack),
				     entropy_worker, &read, NULL, NULL, K_PRIO_PREEMPT(0), 0,
				     K_NO_WAIT);

	zassert_ok(k_thread_join(tid, K_FOREVER));
	zassert_ok(read.result);
}

ZTEST(windows_entropy, test_seeded_entropy)
{
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	uint8_t first[48];
	uint8_t repeated[48];
	uint8_t different[48];

	zassert_true(device_is_ready(dev));
	nsi_host_srandom(41U);
	read_on_worker(dev, first);
	nsi_host_srandom(41U);
	read_on_worker(dev, repeated);
	zassert_mem_equal(first, repeated, sizeof(first));
	nsi_host_srandom(42U);
	read_on_worker(dev, different);
	zassert_not_equal(memcmp(first, different, sizeof(first)), 0);
}

ZTEST(windows_entropy, test_random_range)
{
	bool upper_bit = false;

	nsi_host_srandom(41U);
	for (uint32_t i = 0U; i < 64U; i++) {
		int32_t value = (int32_t)nsi_host_random();

		zassert_true(value >= 0 && value <= INT32_MAX);
		upper_bit |= ((uint32_t)value & (1U << 30U)) != 0U;
	}
	zassert_true(upper_bit, "The native driver needs 31-bit random values");
}

ZTEST_SUITE(windows_entropy, NULL, NULL, NULL, NULL, NULL);
