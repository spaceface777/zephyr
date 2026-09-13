/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "nsi_tasks.h"

#ifdef __APPLE__

#define NSI_MACHO_TASK_RANGE(name, section_name) \
	extern const struct nsi_macho_task __nsi_##name##_tasks_start[] \
		__asm("section$start$__ZNSITASK$" section_name); \
	extern const struct nsi_macho_task __nsi_##name##_tasks_end[] \
		__asm("section$end$__ZNSITASK$" section_name)

NSI_MACHO_TASK_RANGE(PRE_BOOT_1, "__s0");
NSI_MACHO_TASK_RANGE(PRE_BOOT_2, "__s1");
NSI_MACHO_TASK_RANGE(HW_INIT, "__s2");
NSI_MACHO_TASK_RANGE(PRE_BOOT_3, "__s3");
NSI_MACHO_TASK_RANGE(FIRST_SLEEP, "__s4");
NSI_MACHO_TASK_RANGE(ON_EXIT_PRE, "__s5");
NSI_MACHO_TASK_RANGE(ON_EXIT_POST, "__s6");

static int task_before(const struct nsi_macho_task *left,
		       const struct nsi_macho_task *right)
{
	if (left->priority != right->priority) {
		return left->priority < right->priority;
	}
	return left < right;
}

/**
 * @brief Run the Mach-O native simulator tasks for one level.
 */
void nsi_run_tasks(int level)
{
	static const struct nsi_macho_task *starts[] = {
		__nsi_PRE_BOOT_1_tasks_start, __nsi_PRE_BOOT_2_tasks_start,
		__nsi_HW_INIT_tasks_start, __nsi_PRE_BOOT_3_tasks_start,
		__nsi_FIRST_SLEEP_tasks_start, __nsi_ON_EXIT_PRE_tasks_start,
		__nsi_ON_EXIT_POST_tasks_start,
	};
	static const struct nsi_macho_task *ends[] = {
		__nsi_PRE_BOOT_1_tasks_end, __nsi_PRE_BOOT_2_tasks_end,
		__nsi_HW_INIT_tasks_end, __nsi_PRE_BOOT_3_tasks_end,
		__nsi_FIRST_SLEEP_tasks_end, __nsi_ON_EXIT_PRE_tasks_end,
		__nsi_ON_EXIT_POST_tasks_end,
	};
	const struct nsi_macho_task *previous = 0;

	while (1) {
		const struct nsi_macho_task *candidate;
		const struct nsi_macho_task *next = 0;

		for (candidate = starts[level]; candidate < ends[level]; candidate++) {
			if (previous != 0 && !task_before(previous, candidate)) {
				continue;
			}
			if (next == 0 || task_before(candidate, next)) {
				next = candidate;
			}
		}
		if (next == 0) {
			break;
		}
		next->function();
		previous = next;
	}
}

#else

/**
 * @brief Run the set of special NSI tasks corresponding to the given level
 *
 * @param level One of NSITASK_*_LEVEL as defined in nsi_tasks.h
 */
void nsi_run_tasks(int level)
{
	extern void (*__nsi_PRE_BOOT_1_tasks_start[])(void);
	extern void (*__nsi_PRE_BOOT_2_tasks_start[])(void);
	extern void (*__nsi_HW_INIT_tasks_start[])(void);
	extern void (*__nsi_PRE_BOOT_3_tasks_start[])(void);
	extern void (*__nsi_FIRST_SLEEP_tasks_start[])(void);
	extern void (*__nsi_ON_EXIT_PRE_tasks_start[])(void);
	extern void (*__nsi_ON_EXIT_POST_tasks_start[])(void);
	extern void (*__nsi_tasks_end[])(void);

	static void (**nsi_pre_tasks[])(void) = {
		__nsi_PRE_BOOT_1_tasks_start,
		__nsi_PRE_BOOT_2_tasks_start,
		__nsi_HW_INIT_tasks_start,
		__nsi_PRE_BOOT_3_tasks_start,
		__nsi_FIRST_SLEEP_tasks_start,
		__nsi_ON_EXIT_PRE_tasks_start,
		__nsi_ON_EXIT_POST_tasks_start,
		__nsi_tasks_end
	};
	void (**fptr)(void);

	for (fptr = nsi_pre_tasks[level]; fptr < nsi_pre_tasks[level + 1]; fptr++) {
		if (*fptr) { /* LCOV_EXCL_BR_LINE */
			(*fptr)();
		}
	}
}

#endif
