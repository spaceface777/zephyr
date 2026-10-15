/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include "nsi_host_trampolines.h"
#include "nsi_utils.h"

/* Native threads execute serially. Keep one test-generator state across them. */
static uint32_t random_state = 1U;

/* Embedding hosts may provide their own random() and srandom(), as on POSIX hosts. */
NSI_WEAK long random(void)
{
	/* xorshift32; discard one bit to match the POSIX random() result range. */
	random_state ^= random_state << 13U;
	random_state ^= random_state >> 17U;
	random_state ^= random_state << 5U;
	return (long)(random_state >> 1U);
}

NSI_WEAK void srandom(unsigned int seed)
{
	/* Zero is the absorbing state of xorshift32. */
	random_state = seed != 0U ? seed : 1U;
}
