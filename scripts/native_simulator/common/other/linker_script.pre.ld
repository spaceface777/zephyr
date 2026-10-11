/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Linker command/script file for the native simulator runner
 */

#if defined(_WIN32)
#define NSI_SYMBOL_(name) _##name
#define NSI_SYMBOL(name) NSI_SYMBOL_(name)
#define NSI_SECTION_ALIGN BLOCK(__section_alignment__)
#else
#define NSI_SYMBOL(name) name
#define NSI_SECTION_ALIGN
#endif

#define NSI_INIT_LEVEL(level)					\
		NSI_SYMBOL(__nsi_##level##_tasks_start) = .;			\
		KEEP(*(SORT(.nsi_##level[0-9]_task)));		\
		KEEP(*(SORT(.nsi_##level[1-9][0-9]_task)));		\
		KEEP(*(SORT(.nsi_##level[1-9][0-9][0-9]_task)));	\

SECTIONS
 {
	nsi_tasks NSI_SECTION_ALIGN :
	{
		NSI_SYMBOL(__nsi_tasks_start) = .;
		NSI_INIT_LEVEL(PRE_BOOT_1)
		NSI_INIT_LEVEL(PRE_BOOT_2)
		NSI_INIT_LEVEL(HW_INIT)
		NSI_INIT_LEVEL(PRE_BOOT_3)
		NSI_INIT_LEVEL(FIRST_SLEEP)
		NSI_INIT_LEVEL(ON_EXIT_PRE)
		NSI_INIT_LEVEL(ON_EXIT_POST)
		NSI_SYMBOL(__nsi_tasks_end) = .;
	}

	nsi_hw_events NSI_SECTION_ALIGN :
	{
		NSI_SYMBOL(__nsi_hw_events_start) = .;
		KEEP(*(SORT(.nsi_hw_event_[0-9])));		\
		KEEP(*(SORT(.nsi_hw_event_[1-9][0-9])));	\
		KEEP(*(SORT(.nsi_hw_event_[1-9][0-9][0-9])));
		NSI_SYMBOL(__nsi_hw_events_end) = .;
	}
 } INSERT AFTER .data;

/*
 * Note this script augments the default host linker script
 */
