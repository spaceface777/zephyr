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

#if defined(_WIN32)
	/*
	 * LLVM's libunwind finds the DWARF unwind tables of a module by the section
	 * name ".eh_frame" cut to the 8 characters of a PE section header. GNU ld
	 * writes the full name to the string table instead, if the image has any
	 * long section names, such as those of its debug information. This rule
	 * takes all unwind tables before the default ".eh_frame" rule does, with the
	 * same input order, as LLD names them. The libgcc unwinder registers its
	 * tables by address, so the name does not matter to GCC.
	 */
	.eh_fram NSI_SECTION_ALIGN :
	{
		KEEP(*(.eh_frame*))
	}
#endif
 } INSERT AFTER .data;

/*
 * Note this script augments the default host linker script
 */
