/*
 * Copyright (c) 2017 Oticon A/S
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Overall HW models scheduler for the native simulator
 *
 * Models events are registered with NSI_HW_EVENT().
 */

#include <stdint.h>
#include <signal.h>
#include <stddef.h>
#include <inttypes.h>
#include "nsi_tracing.h"
#include "nsi_main.h"
#include "nsi_hw_scheduler.h"
#include "nsi_hws_models_if.h"

uint64_t nsi_simu_time; /* The actual time as known by the HW models */
static uint64_t end_of_time = NSI_NEVER; /* When will this device stop */

#ifdef __APPLE__
extern struct nsi_hw_event_st __nsi_hw_events_start[]
	__asm("section$start$__ZNSIEVT$__events");
extern struct nsi_hw_event_st __nsi_hw_events_end[]
	__asm("section$end$__ZNSIEVT$__events");
#else
extern struct nsi_hw_event_st __nsi_hw_events_start[];
extern struct nsi_hw_event_st __nsi_hw_events_end[];
#endif

static unsigned int number_of_events;

static unsigned int next_timer_index;
static uint64_t next_timer_time;

/* Have we received a SIGTERM or SIGINT */
static volatile sig_atomic_t signaled_end;

void nsi_hws_request_stop(void)
{
	signaled_end = 1;
}


static void nsi_hws_sleep_until_next_event(void)
{
	if (next_timer_time >= nsi_simu_time) { /* LCOV_EXCL_BR_LINE */
		nsi_simu_time = next_timer_time;
	} else {
		/* LCOV_EXCL_START */
		nsi_print_warning("next_timer_time corrupted (%"PRIu64"<= %"
				PRIu64", timer idx=%i)\n",
				(uint64_t)next_timer_time,
				(uint64_t)nsi_simu_time,
				next_timer_index);
		/* LCOV_EXCL_STOP */
	}

	if (signaled_end || (nsi_simu_time > end_of_time)) {
		nsi_print_trace("\nStopped at %.3Lfs\n",
				((long double)nsi_simu_time)/1.0e6L);
		nsi_exit(0);
	}
}


/**
 * Find in between all events timers which is the next one.
 * (and update the internal next_timer_* accordingly)
 */
void nsi_hws_find_next_event(void)
{
	next_timer_index = 0;
	next_timer_time  = *__nsi_hw_events_start[0].timer;

	for (unsigned int i = 1; i < number_of_events ; i++) {
		if (next_timer_time > *__nsi_hw_events_start[i].timer) {
			next_timer_index = i;
			next_timer_time = *__nsi_hw_events_start[i].timer;
		}
	}
}

uint64_t nsi_hws_get_next_event_time(void)
{
	return next_timer_time;
}

/**
 * Execute the next scheduled HW event
 * (advancing time until that event would trigger)
 */
void nsi_hws_one_event(void)
{
	nsi_hws_sleep_until_next_event();

	if (next_timer_index < number_of_events) { /* LCOV_EXCL_BR_LINE */
		__nsi_hw_events_start[next_timer_index].callback();
	} else {
		nsi_print_error_and_exit("next_timer_index corrupted\n"); /* LCOV_EXCL_LINE */
	}

	nsi_hws_find_next_event();
}

/**
 * Set the simulated time when the process will stop
 */
void nsi_hws_set_end_of_time(uint64_t new_end_of_time)
{
	end_of_time = new_end_of_time;
}

/**
 * Function to initialize the HW scheduler
 *
 * Note that the HW models should register their initialization functions
 * as NSI_TASKS of HW_INIT level.
 */
void nsi_hws_init(void)
{
	number_of_events = __nsi_hw_events_end - __nsi_hw_events_start;

	nsi_hws_find_next_event();
}

/**
 * Function to free any resources allocated by the HW scheduler
 *
 * Note that the HW models should register their initialization functions
 * as NSI_TASKS of ON_EXIT_PRE/POST levels.
 */
void nsi_hws_cleanup(void)
{
	/* Nothing to be done so far */
}
