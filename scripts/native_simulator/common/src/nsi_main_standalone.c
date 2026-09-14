/*
 * Copyright (c) 2017 Oticon A/S
 * Copyright (c) 2023 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

/* Process policy for the standalone runner, separate from reusable initialization. */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include "nsi_main.h"
#include "nsi_hw_scheduler.h"
#include "nsi_safe_call.h"

NSI_FUNC_NORETURN void nsi_exit(int exit_code)
{
	exit(nsi_exit_inner(exit_code));
}

#ifndef NSI_NO_MAIN
static void signal_end(int sig)
{
	(void)sig;
	nsi_hws_request_stop();
}

int main(int argc, char *argv[])
{
	struct sigaction act = {0};

	/* A second signal retains the default action if execution cannot yield. */
	act.sa_handler = signal_end;
	act.sa_flags = SA_RESETHAND;
	NSI_SAFE_CALL(sigemptyset(&act.sa_mask));
	NSI_SAFE_CALL(sigaction(SIGTERM, &act, NULL));
	NSI_SAFE_CALL(sigaction(SIGINT, &act, NULL));
	setvbuf(stdout, NULL, _IOLBF, 512);
	setvbuf(stderr, NULL, _IOLBF, 512);

	nsi_init(argc, argv);
	for (;;) {
		nsi_hws_one_event();
	}
}
#endif /* NSI_NO_MAIN */
