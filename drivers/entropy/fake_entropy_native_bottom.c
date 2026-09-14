/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bottom/Linux side of the pseudo-random entropy generator for the native simulator
 */

#undef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700

#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#ifndef __APPLE__
#include <sys/random.h>
#endif
#include "nsi_tracing.h"
#ifdef QSIM_EMBEDDED_RUNNER
#include "nsi_host_trampolines.h"
#endif

void entropy_native_seed(unsigned int seed, bool seed_random)
{
	if (seed_random == false) {
#ifdef QSIM_EMBEDDED_RUNNER
		nsi_host_srandom(seed);
#else
		srandom(seed);
#endif
	} else {
#ifdef QSIM_EMBEDDED_RUNNER
		nsi_print_error_and_exit("Random host seeding is unsupported in embedded native_sim\n");
#else
		unsigned int buf;
#ifdef __APPLE__
		arc4random_buf(&buf, sizeof(buf));
#else
		int err = getrandom(&buf, sizeof(buf), 0);

		if (err != sizeof(buf)) {
			nsi_print_error_and_exit("Could not get random number (%i, %s)\n",
						 err, strerror(errno));
		}
#endif
		srandom(buf);

		/* Let's print the seed so users can still reproduce the run if they need to */
		nsi_print_trace("Random generator seeded with 0x%X\n", buf);
#endif
	}
}
