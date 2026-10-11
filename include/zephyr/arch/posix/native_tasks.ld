/*
 * Copyright (c) 2018 Oticon A/S
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define NATIVE_INIT_LEVEL(level)					\
		PLACE_SYMBOL_HERE(__native_##level##_tasks_start);	\
		KEEP(*(SORT(.native_##level[0-9]_task)));		\
		KEEP(*(SORT(.native_##level[1-9][0-9]_task)));		\
		KEEP(*(SORT(.native_##level[1-9][0-9][0-9]_task)));	\

SECTION_PROLOGUE (native_pre_tasks,,)
{
	PLACE_SYMBOL_HERE(__native_tasks_start);
	NATIVE_INIT_LEVEL(PRE_BOOT_1)
	NATIVE_INIT_LEVEL(PRE_BOOT_2)
	NATIVE_INIT_LEVEL(PRE_BOOT_3)
	NATIVE_INIT_LEVEL(FIRST_SLEEP)
	NATIVE_INIT_LEVEL(ON_EXIT)
	PLACE_SYMBOL_HERE(__native_tasks_end);
}
