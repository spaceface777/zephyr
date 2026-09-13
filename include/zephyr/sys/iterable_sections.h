/*
 * Copyright (C) 2020, Intel Corporation
 * Copyright (C) 2023, Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef INCLUDE_ZEPHYR_SYS_ITERABLE_SECTIONS_H_
#define INCLUDE_ZEPHYR_SYS_ITERABLE_SECTIONS_H_

#include <zephyr/sys/__assert.h>
#include <zephyr/toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Iterable Sections APIs
 * @defgroup iterable_section_apis Iterable Sections APIs
 * @ingroup os_services
 * @{
 */

/**
 * @brief Defines a new element for an iterable section for a generic type.
 *
 * @details
 * Convenience helper combining __in_section() and Z_DECL_ALIGN().
 * The section name will be '.[SECNAME].static.[SECTION_POSTFIX]'
 *
 * In the linker script, create output sections for these using
 * ITERABLE_SECTION_ROM() or ITERABLE_SECTION_RAM().
 *
 * @note In order to store the element in ROM, a const specifier has to
 * be added to the declaration: const TYPE_SECTION_ITERABLE(...);
 *
 * @param[in]  type data type of variable
 * @param[in]  varname name of variable to place in section
 * @param[in]  secname type name of iterable section.
 * @param[in]  section_postfix postfix to use in section name
 */
#if defined(ZEPHYR_TARGET_MACHO)
#define Z_MACHO_ITER_SECTION_device __device
#define Z_MACHO_ITER_SECTION__static_thread_data __static_threads
#define Z_MACHO_ITER_SECTION_k_timer __k_timer
#define Z_MACHO_ITER_SECTION_k_mem_slab __k_mem_slab
#define Z_MACHO_ITER_SECTION_k_heap __k_heap
#define Z_MACHO_ITER_SECTION_k_mutex __k_mutex
#define Z_MACHO_ITER_SECTION_k_stack __k_stack
#define Z_MACHO_ITER_SECTION_k_msgq __k_msgq
#define Z_MACHO_ITER_SECTION_k_mbox __k_mbox
#define Z_MACHO_ITER_SECTION_k_pipe __k_pipe
#define Z_MACHO_ITER_SECTION_k_sem __k_sem
#define Z_MACHO_ITER_SECTION_k_event __k_event
#define Z_MACHO_ITER_SECTION_k_queue __k_queue
#define Z_MACHO_ITER_SECTION_k_fifo __k_fifo
#define Z_MACHO_ITER_SECTION_k_lifo __k_lifo
#define Z_MACHO_ITER_SECTION_k_condvar __k_condvar
#define Z_MACHO_ITER_SECTION_log_const __log_const
#define Z_MACHO_ITER_SECTION_log_dynamic __log_dynamic
#define Z_MACHO_ITER_SECTION_log_msg_ptr __log_msg_ptr
#define Z_MACHO_ITER_SECTION_log_mpsc_pbuf __log_mpsc_pbuf
#define Z_MACHO_ITER_SECTION_log_backend __log_backend
#define Z_MACHO_ITER_SECTION_log_link __log_link
#define Z_MACHO_ITER_SECTION_shell __shell
#define Z_MACHO_ITER_SECTION_shell_root_cmds __shell_root
#define Z_MACHO_ITER_SECTION_shell_subcmds __shell_subcmd
#define Z_MACHO_ITER_SECTION_shell_dynamic_subcmds __shell_dynamic
#define Z_MACHO_ITER_SECTION_rtio __rtio
#define Z_MACHO_ITER_SECTION_rtio_pool __rtio_pool
#define Z_MACHO_ITER_SECTION_rtio_iodev __rtio_iodev
#define Z_MACHO_ITER_SECTION_rtio_sqe_pool __rtio_sqe_pool
#define Z_MACHO_ITER_SECTION_rtio_cqe_pool __rtio_cqe_pool
#define Z_MACHO_ITER_SECTION_zbus_channel __zbus_channel
#define Z_MACHO_ITER_SECTION_zbus_channel_observation __zbus_chan_obs
#define Z_MACHO_ITER_SECTION_zbus_channel_observation_mask __zbus_obs_mask
#define Z_MACHO_ITER_SECTION_zbus_observer __zbus_observer
#define Z_MACHO_ITER_SECTION_zbus_shadow_channel __zbus_shadow
#define Z_MACHO_ITER_SECTION_entropy_driver_api __api_entropy
#define Z_MACHO_ITER_SECTION_uart_driver_api __api_uart
#define Z_MACHO_ITER_SECTION_net_buf_pool __net_buf_pool
#define Z_MACHO_ITER_SECTION_sys_mem_blocks_ptr __sys_mem_blocks
#define Z_MACHO_ITER_SECTION_(secname) Z_MACHO_ITER_SECTION_##secname
#define Z_MACHO_ITER_SECTION(secname) Z_MACHO_ITER_SECTION_(secname)
#define Z_MACHO_ITER_SECTION_STRING(secname) STRINGIFY(Z_MACHO_ITER_SECTION(secname))

#define TYPE_SECTION_ITERABLE(type, varname, secname, section_postfix) \
	Z_DECL_ALIGN(type) varname \
	__attribute__((section("__ZITER," Z_MACHO_ITER_SECTION_STRING(secname)))) \
	__used __noasan
#else
#define TYPE_SECTION_ITERABLE(type, varname, secname, section_postfix) \
	Z_DECL_ALIGN(type) varname \
	__in_section(_##secname, static, _CONCAT(section_postfix, _)) __used __noasan
#endif

/**
 * @brief iterable section start symbol for a generic type
 *
 * will return '_[OUT_TYPE]_list_start'.
 *
 * @param[in]  secname type name of iterable section.  For 'struct foobar' this
 * would be TYPE_SECTION_START(foobar)
 *
 */
#define TYPE_SECTION_START(secname) _CONCAT(_##secname, _list_start)

/**
 * @brief iterable section end symbol for a generic type
 *
 * will return '_<SECNAME>_list_end'.
 *
 * @param[in]  secname type name of iterable section.  For 'struct foobar' this
 * would be TYPE_SECTION_START(foobar)
 */
#define TYPE_SECTION_END(secname) _CONCAT(_##secname, _list_end)

/**
 * @brief iterable section extern for start symbol for a generic type
 *
 * Helper macro to give extern for start of iterable section.  The macro
 * typically will be called TYPE_SECTION_START_EXTERN(struct foobar, foobar).
 * This allows the macro to hand different types as well as cases where the
 * type and section name may differ.
 *
 * @param[in]  type data type of section
 * @param[in]  secname name of output section
 */
#if defined(ZEPHYR_TARGET_MACHO)
#define TYPE_SECTION_START_EXTERN(type, secname) \
	extern type TYPE_SECTION_START(secname)[] \
	__asm("section$start$__ZITER$" Z_MACHO_ITER_SECTION_STRING(secname))
#else
#define TYPE_SECTION_START_EXTERN(type, secname) \
	extern type TYPE_SECTION_START(secname)[]
#endif

/**
 * @brief iterable section extern for end symbol for a generic type
 *
 * Helper macro to give extern for end of iterable section.  The macro
 * typically will be called TYPE_SECTION_END_EXTERN(struct foobar, foobar).
 * This allows the macro to hand different types as well as cases where the
 * type and section name may differ.
 *
 * @param[in]  type data type of section
 * @param[in]  secname name of output section
 */
#if defined(ZEPHYR_TARGET_MACHO)
#define TYPE_SECTION_END_EXTERN(type, secname) \
	extern type TYPE_SECTION_END(secname)[] \
	__asm("section$end$__ZITER$" Z_MACHO_ITER_SECTION_STRING(secname))
#else
#define TYPE_SECTION_END_EXTERN(type, secname) \
	extern type TYPE_SECTION_END(secname)[]
#endif

/**
 * @brief Iterate over a specified iterable section for a generic type
 *
 * @details
 * Iterator for structure instances gathered by TYPE_SECTION_ITERABLE().
 * The linker must provide a _<SECNAME>_list_start symbol and a
 * _<SECNAME>_list_end symbol to mark the start and the end of the
 * list of struct objects to iterate over. This is normally done using
 * ITERABLE_SECTION_ROM() or ITERABLE_SECTION_RAM() in the linker script.
 */
#define TYPE_SECTION_FOREACH(type, secname, iterator)		\
	TYPE_SECTION_START_EXTERN(type, secname);		\
	TYPE_SECTION_END_EXTERN(type, secname);		\
	for (type * iterator = TYPE_SECTION_START(secname); ({	\
		__ASSERT(iterator <= TYPE_SECTION_END(secname),\
			      "unexpected list end location");	\
		     iterator < TYPE_SECTION_END(secname);	\
	     });						\
	     iterator++)

/**
 * @brief Get element from section for a generic type.
 *
 * @note There is no protection against reading beyond the section.
 *
 * @param[in]  type type of element
 * @param[in]  secname name of output section
 * @param[in]  i Index.
 * @param[out] dst Pointer to location where pointer to element is written.
 */
#define TYPE_SECTION_GET(type, secname, i, dst) do { \
	TYPE_SECTION_START_EXTERN(type, secname); \
	*(dst) = &TYPE_SECTION_START(secname)[i]; \
} while (0)

/**
 * @brief Count elements in a section for a generic type.
 *
 * @param[in]  type type of element
 * @param[in]  secname name of output section
 * @param[out] dst Pointer to location where result is written.
 */
#define TYPE_SECTION_COUNT(type, secname, dst) do { \
	TYPE_SECTION_START_EXTERN(type, secname); \
	TYPE_SECTION_END_EXTERN(type, secname); \
	*(dst) = ((uintptr_t)TYPE_SECTION_END(secname) - \
		  (uintptr_t)TYPE_SECTION_START(secname)) / sizeof(type); \
} while (0)

/**
 * @brief iterable section start symbol for a struct type
 *
 * @param[in]  struct_type data type of section
 */
#define STRUCT_SECTION_START(struct_type) \
	TYPE_SECTION_START(struct_type)

/**
 * @brief iterable section extern for start symbol for a struct
 *
 * Helper macro to give extern for start of iterable section.
 *
 * @param[in]  struct_type data type of section
 */
#define STRUCT_SECTION_START_EXTERN(struct_type) \
	TYPE_SECTION_START_EXTERN(struct struct_type, struct_type)

/**
 * @brief iterable section end symbol for a struct type
 *
 * @param[in]  struct_type data type of section
 */
#define STRUCT_SECTION_END(struct_type) \
	TYPE_SECTION_END(struct_type)

/**
 * @brief iterable section extern for end symbol for a struct
 *
 * Helper macro to give extern for end of iterable section.
 *
 * @param[in]  struct_type data type of section
 */
#define STRUCT_SECTION_END_EXTERN(struct_type) \
	TYPE_SECTION_END_EXTERN(struct struct_type, struct_type)

/**
 * @brief Defines a new element of alternate data type for an iterable section.
 *
 * @details
 * Special variant of STRUCT_SECTION_ITERABLE(), for placing alternate
 * data types within the iterable section of a specific data type. The
 * data type sizes and semantics must be equivalent!
 */
#define STRUCT_SECTION_ITERABLE_ALTERNATE(secname, struct_type, varname) \
	TYPE_SECTION_ITERABLE(struct struct_type, varname, secname, varname)

/**
 * @brief Defines an array of elements of alternate data type for an iterable
 * section.
 *
 * @see STRUCT_SECTION_ITERABLE_ALTERNATE
 */
#define STRUCT_SECTION_ITERABLE_ARRAY_ALTERNATE(secname, struct_type, varname, \
						size)                          \
	TYPE_SECTION_ITERABLE(struct struct_type, varname[size], secname,      \
			      varname)

/**
 * @brief Defines a new element for an iterable section.
 *
 * @details
 * Convenience helper combining __in_section() and Z_DECL_ALIGN().
 * The section name is the struct type prepended with an underscore.
 * The subsection is "static" and the subsubsection is the variable name.
 *
 * In the linker script, create output sections for these using
 * ITERABLE_SECTION_ROM() or ITERABLE_SECTION_RAM().
 *
 * @note In order to store the element in ROM, a const specifier has to
 * be added to the declaration: const STRUCT_SECTION_ITERABLE(...);
 */
#define STRUCT_SECTION_ITERABLE(struct_type, varname) \
	STRUCT_SECTION_ITERABLE_ALTERNATE(struct_type, struct_type, varname)

/**
 * @brief Defines an array of elements for an iterable section.
 *
 * @see STRUCT_SECTION_ITERABLE
 */
#define STRUCT_SECTION_ITERABLE_ARRAY(struct_type, varname, size)              \
	STRUCT_SECTION_ITERABLE_ARRAY_ALTERNATE(struct_type, struct_type,      \
						varname, size)

/**
 * @brief Defines a new element for an iterable section with a custom name.
 *
 * The name can be used to customize how iterable section entries are sorted.
 * @see STRUCT_SECTION_ITERABLE()
 */
#define STRUCT_SECTION_ITERABLE_NAMED(struct_type, name, varname) \
	TYPE_SECTION_ITERABLE(struct struct_type, varname, struct_type, name)

/**
 * @brief Defines a new element for an iterable section with a custom name,
 * placed in a custom section.
 *
 * The name can be used to customize how iterable section entries are sorted.
 * @see STRUCT_SECTION_ITERABLE_NAMED()
 */
#define STRUCT_SECTION_ITERABLE_NAMED_ALTERNATE(struct_type, secname, name, varname) \
	TYPE_SECTION_ITERABLE(struct struct_type, varname, secname, name)

/**
 * @brief Iterate over a specified iterable section (alternate).
 *
 * @details
 * Iterator for structure instances gathered by STRUCT_SECTION_ITERABLE().
 * The linker must provide a _<SECNAME>_list_start symbol and a
 * _<SECNAME>_list_end symbol to mark the start and the end of the
 * list of struct objects to iterate over. This is normally done using
 * ITERABLE_SECTION_ROM() or ITERABLE_SECTION_RAM() in the linker script.
 */
#define STRUCT_SECTION_FOREACH_ALTERNATE(secname, struct_type, iterator) \
	TYPE_SECTION_FOREACH(struct struct_type, secname, iterator)

/**
 * @brief Iterate over a specified iterable section.
 *
 * @details
 * Iterator for structure instances gathered by STRUCT_SECTION_ITERABLE().
 * The linker must provide a _<struct_type>_list_start symbol and a
 * _<struct_type>_list_end symbol to mark the start and the end of the
 * list of struct objects to iterate over. This is normally done using
 * ITERABLE_SECTION_ROM() or ITERABLE_SECTION_RAM() in the linker script.
 */
#define STRUCT_SECTION_FOREACH(struct_type, iterator) \
	STRUCT_SECTION_FOREACH_ALTERNATE(struct_type, struct_type, iterator)

/**
 * @brief Get element from section.
 *
 * @note There is no protection against reading beyond the section.
 *
 * @param[in]  struct_type Struct type.
 * @param[in]  i Index.
 * @param[out] dst Pointer to location where pointer to element is written.
 */
#define STRUCT_SECTION_GET(struct_type, i, dst) \
	TYPE_SECTION_GET(struct struct_type, struct_type, i, dst)

/**
 * @brief Count elements in a section.
 *
 * @param[in]  struct_type Struct type
 * @param[out] dst Pointer to location where result is written.
 */
#define STRUCT_SECTION_COUNT(struct_type, dst) \
	TYPE_SECTION_COUNT(struct struct_type, struct_type, dst);

/**
 * @}
 */ /* end of struct_section_apis */

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_ZEPHYR_SYS_ITERABLE_SECTIONS_H_ */
