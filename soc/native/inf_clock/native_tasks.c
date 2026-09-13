/*
 * Copyright (c) 2017 Oticon A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "posix_native_task.h"

#ifdef __APPLE__

#define Z_MACHO_NATIVE_RANGE(name, section_name) \
	extern const struct native_macho_task __native_##name##_tasks_start[] \
		__asm("section$start$__ZNATIVE$" section_name); \
	extern const struct native_macho_task __native_##name##_tasks_end[] \
		__asm("section$end$__ZNATIVE$" section_name)

Z_MACHO_NATIVE_RANGE(PRE_BOOT_1, "__n0");
Z_MACHO_NATIVE_RANGE(PRE_BOOT_2, "__n1");
Z_MACHO_NATIVE_RANGE(PRE_BOOT_3, "__n2");
Z_MACHO_NATIVE_RANGE(FIRST_SLEEP, "__n3");
Z_MACHO_NATIVE_RANGE(ON_EXIT, "__n4");

static int task_before(const struct native_macho_task *left,
		       const struct native_macho_task *right)
{
	if (left->priority != right->priority) {
		return left->priority < right->priority;
	}
	return left < right;
}

/**
 * @brief Run the Mach-O embedded native tasks for one level.
 */
void run_native_tasks(int level)
{
	static const struct native_macho_task *starts[] = {
		__native_PRE_BOOT_1_tasks_start, __native_PRE_BOOT_2_tasks_start,
		__native_PRE_BOOT_3_tasks_start, __native_FIRST_SLEEP_tasks_start,
		__native_ON_EXIT_tasks_start,
	};
	static const struct native_macho_task *ends[] = {
		__native_PRE_BOOT_1_tasks_end, __native_PRE_BOOT_2_tasks_end,
		__native_PRE_BOOT_3_tasks_end, __native_FIRST_SLEEP_tasks_end,
		__native_ON_EXIT_tasks_end,
	};
	const struct native_macho_task *previous = 0;

	while (1) {
		const struct native_macho_task *candidate;
		const struct native_macho_task *next = 0;

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
 * @brief Run the set of special native tasks corresponding to the given level
 *
 * @param level One of _NATIVE_*_LEVEL as defined in soc.h
 */
void run_native_tasks(int level)
{
	extern void (*__native_PRE_BOOT_1_tasks_start[])(void);
	extern void (*__native_PRE_BOOT_2_tasks_start[])(void);
	extern void (*__native_PRE_BOOT_3_tasks_start[])(void);
	extern void (*__native_FIRST_SLEEP_tasks_start[])(void);
	extern void (*__native_ON_EXIT_tasks_start[])(void);
	extern void (*__native_tasks_end[])(void);

	static void (**native_pre_tasks[])(void) = {
		__native_PRE_BOOT_1_tasks_start,
		__native_PRE_BOOT_2_tasks_start,
		__native_PRE_BOOT_3_tasks_start,
		__native_FIRST_SLEEP_tasks_start,
		__native_ON_EXIT_tasks_start,
		__native_tasks_end
	};
	void (**fptr)(void);

	for (fptr = native_pre_tasks[level]; fptr < native_pre_tasks[level + 1]; fptr++) {
		if (*fptr) { /* LCOV_EXCL_BR_LINE */
			(*fptr)();
		}
	}
}

#endif
