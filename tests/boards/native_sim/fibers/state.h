/* SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FIBERS_STATE_H
#define FIBERS_STATE_H
#include <stdint.h>
#include <stddef.h>
struct host_state {
	int err;
	uint16_t cw, sw;
	uint32_t mxcsr, owner;
	uintptr_t errno_addr;
};
int host_set_state(struct host_state *state, uint32_t id);
int host_check_state(const struct host_state *state);
int host_bounds(uintptr_t addr, size_t size, uint32_t sentinel);
uint32_t host_thread_id(void);
void host_wrong_owner(void);
void host_borrow_root(void);
struct host_cleanup {
	uint32_t slot, owner, registered, callbacks, errors;
};
void host_capture_state(struct host_state *state);
int host_saved_errno(void);
int host_cleanup_open(struct host_cleanup *cleanup);
int host_cleanup_register(struct host_cleanup *cleanup);
int host_cleanup_close(struct host_cleanup *cleanup);
#endif
