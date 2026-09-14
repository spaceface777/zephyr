#ifndef NSI_CONTEXT_IF_H
#define NSI_CONTEXT_IF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NSI_CONTEXT_ABI_VERSION 2u

typedef struct nsi_context nsi_context_t;

typedef struct nsi_context_abi_header {
	uint32_t version;
	uint32_t size;
} nsi_context_abi_header_t;

typedef enum nsi_context_status {
	NSI_CONTEXT_OK = 0,
	NSI_CONTEXT_INVALID_ARGUMENT = 1,
	NSI_CONTEXT_ABI_MISMATCH = 2,
	NSI_CONTEXT_WRONG_STATE = 3,
	NSI_CONTEXT_NO_MEMORY = 4,
	NSI_CONTEXT_UNSUPPORTED = 5,
} nsi_context_status_t;

typedef enum nsi_context_role {
	NSI_CONTEXT_SIMULATOR_ROOT = 0,
	NSI_CONTEXT_MODULE_DRIVER = 1,
	NSI_CONTEXT_ZEPHYR_BOOTSTRAP = 2,
	NSI_CONTEXT_ZEPHYR_THREAD = 3,
} nsi_context_role_t;

typedef void (*nsi_context_entry_fn)(void *argument);

typedef struct nsi_context_stack {
	void *usable_begin;
	size_t usable_size;
} nsi_context_stack_t;

typedef nsi_context_status_t (*nsi_context_attach_root_fn)(void *user, nsi_context_t **root);
typedef nsi_context_status_t (*nsi_context_create_fn)(void *user, uint64_t owner,
						      nsi_context_role_t role,
						      nsi_context_stack_t stack,
						      nsi_context_entry_fn entry, void *argument,
						      nsi_context_t **context);
typedef nsi_context_status_t (*nsi_context_switch_fn)(void *user, nsi_context_t *from,
						      nsi_context_t *to);
typedef nsi_context_status_t (*nsi_context_abandon_fn)(void *user, nsi_context_t *from,
						       nsi_context_t *to);
typedef nsi_context_status_t (*nsi_context_destroy_fn)(void *user, nsi_context_t *context);
typedef nsi_context_status_t (*nsi_context_stack_bounds_fn)(void *user,
							    const nsi_context_t *context,
							    nsi_context_stack_t *stack);
typedef nsi_context_status_t (*nsi_context_stack_allocate_fn)(void *user, size_t usable_size,
							      nsi_context_stack_t *stack);
typedef nsi_context_status_t (*nsi_context_stack_release_fn)(void *user, nsi_context_stack_t stack);

typedef struct nsi_context_if {
	nsi_context_abi_header_t header;
	void *user;
	nsi_context_attach_root_fn attach_root;
	nsi_context_create_fn create;
	nsi_context_switch_fn switch_to;
	nsi_context_abandon_fn abandon_to;
	nsi_context_destroy_fn destroy;
	nsi_context_stack_bounds_fn stack_bounds;
	nsi_context_stack_allocate_fn allocate_stack;
	nsi_context_stack_release_fn release_stack;
} nsi_context_if_t;

/*
 * Bind the instance-private NCT/NCE adapters to their host context service.
 * The implementation of these functions is linked into each materialized
 * runner, so the binding and active-context state are private to one MCU.
 */
nsi_context_status_t nsi_context_bind(const nsi_context_if_t *interface, uint64_t owner,
				      nsi_context_t *owner_context, size_t native_stack_size);
nsi_context_status_t nsi_context_unbind(void);

static inline nsi_context_status_t nsi_context_check_abi(const nsi_context_if_t *interface)
{
	if (interface == NULL) {
		return NSI_CONTEXT_INVALID_ARGUMENT;
	}
	if (interface->header.version != NSI_CONTEXT_ABI_VERSION ||
	    interface->header.size < sizeof(*interface)) {
		return NSI_CONTEXT_ABI_MISMATCH;
	}
	return NSI_CONTEXT_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* NSI_CONTEXT_IF_H */
