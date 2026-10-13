/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef WINDOWS_COROUTINES_HOST_H
#define WINDOWS_COROUTINES_HOST_H

#include <stddef.h>
#include <stdint.h>

struct host_state {
	int err;
	uint16_t cw;
	uint16_t sw;
	uint32_t mxcsr;
	uint32_t owner;
	uintptr_t errno_addr;
};

int host_set_state(struct host_state *state, uint32_t id);
int host_check_state(const struct host_state *state);
int host_bounds(uintptr_t addr, size_t size, uint32_t sentinel);
int host_current_stack(uintptr_t addr, size_t size);
int host_raise_handled(void);
int host_stack_released(uintptr_t addr);
uint32_t host_thread_id(void);

#endif /* WINDOWS_COROUTINES_HOST_H */
