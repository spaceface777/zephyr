/*
 * Copyright (c) 2017 Oticon A/S
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Native simulator entry point (main), and the policy which belongs to the
 * native simulator owning the whole process: stdio buffering, termination
 * signals, and process exit.
 *
 * A program which embeds the runner and drives it itself can leave this file
 * out of the build (see NSI_EXCLUDE_SRCS in the Makefile), call
 * nsi_init_until_boot() and nsi_boot() instead of nsi_init(), and provide its
 * own nsi_exit().
 *
 * Documentation can be found starting in docs/README.md
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "nsi_main.h"
#include "nsi_main_semipublic.h"
#include "nsi_hw_scheduler.h"
#include "nsi_safe_call.h"

NSI_FUNC_NORETURN void nsi_exit(int exit_code)
{
	exit(nsi_exit_inner(exit_code));
}

/**
 * Handler for SIGTERM and SIGINT
 */
static void nsi_signal_end_handler(int sig)
{
	nsi_hws_request_stop();
}

/**
 * Set the handler for SIGTERM and SIGINT which will cause the
 * program to exit gracefully when they are received the 1st time
 *
 * Note that our handler only sets a variable indicating the signal was
 * received, and in each iteration of the hw main loop this variable is
 * evaluated.
 * If for some reason (the program is stuck) we never evaluate it, the program
 * would never exit.
 * Therefore we set SA_RESETHAND: This way, the 2nd time the signal is received
 * the default handler would be called to terminate the program no matter what.
 *
 * Note that SA_RESETHAND requires either _POSIX_C_SOURCE>=200809L or
 * _XOPEN_SOURCE>=500
 */
static void nsi_set_sig_handler(void)
{
	struct sigaction act;

	act.sa_handler = nsi_signal_end_handler;
	NSI_SAFE_CALL(sigemptyset(&act.sa_mask));

	act.sa_flags = SA_RESETHAND;

	NSI_SAFE_CALL(sigaction(SIGTERM, &act, NULL));
	NSI_SAFE_CALL(sigaction(SIGINT, &act, NULL));
}

/**
 * Run all early native simulator initialization steps, including command
 * line parsing and CPU start, until we are ready to let the HW models
 * run via nsi_hws_one_event()
 *
 * Note: This API should normally only be called by the native simulator main()
 */
void nsi_init(int argc, char *argv[])
{
	/*
	 * Let's ensure that even if we are redirecting to a file, we get stdout
	 * and stderr line buffered (default for console)
	 * Note that glibc ignores size. But just in case we set a reasonable
	 * number in case somebody tries to compile against a different library
	 */
	setvbuf(stdout, NULL, _IOLBF, 512);
	setvbuf(stderr, NULL, _IOLBF, 512);

	nsi_set_sig_handler();

	nsi_init_until_boot(argc, argv);
	nsi_boot();
}

#ifndef NSI_NO_MAIN

/**
 *
 * Note that this main() is not used when building fuzz cases,
 * as libfuzzer has its own main(),
 * and calls the "OS" through a per-case fuzz test entry point.
 */
int main(int argc, char *argv[])
{
	nsi_init(argc, argv);
	while (true) {
		nsi_hws_one_event();
	}

	NSI_CODE_UNREACHABLE; /* LCOV_EXCL_LINE */
}

#endif /* NSI_NO_MAIN */
