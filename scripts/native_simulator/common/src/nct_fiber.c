/*
 * Copyright (c) 2017 Oticon A/S
 * Copyright (c) 2023 Nordic Semiconductor ASA
 * Copyright (c) 2026 Raul Hernandez
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Native simulator, CPU thread emulation using userspace contexts. */

#undef _GNU_SOURCE
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nsi_utils.h"
#include "nct_if.h"
#include "nsi_internal.h"
#include "nct_context.h"

#if defined(__SANITIZE_ADDRESS__)
#error "The NCT fiber backend is not compatible with AddressSanitizer yet"
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(memory_sanitizer)
#error "The NCT fiber backend is not compatible with stack-tracking sanitizers yet"
#endif
#endif

#ifndef NSI_NCT_FIBER_STACK_SIZE
#define NSI_NCT_FIBER_STACK_SIZE (1024U * 1024U)
#endif

#define NCT_ALLOC_CHUNK_SIZE 64
#define NCT_REUSE_ABORTED_ENTRIES 0
#define NCT_THREAD_NAME_LEN 16

struct nct_status_t;

struct threads_table_el {
	struct nct_status_t *nct_status;
	struct threads_table_el *next;
	struct nct_context context;
	void *stack_mapping;
	size_t stack_mapping_size;
	void *stack_addr;
	size_t stack_size;
	void *payload;
	int saved_errno;
	int thread_idx;
	int thread_cnt;
	char name[NCT_THREAD_NAME_LEN];
	enum { NOTUSED = 0, USED, ABORTING, ABORTED, FAILED } state;
	bool running;
};

struct nct_status_t {
	struct threads_table_el *threads_table;
	int thread_create_count;
	int threads_table_size;
	void (*fptr)(void *payload);
	int currently_allowed_thread;
	struct nct_context root_context;
	int root_errno;
	bool terminate;
};

static struct threads_table_el *ttable_get_element(struct nct_status_t *this, int index);

static void ttable_init_elements(struct threads_table_el *chunk, int size)
{
	for (int i = 0; i < size - 1; i++) {
		chunk[i].next = &chunk[i + 1];
	}
	chunk[size - 1].next = NULL;
}

static struct threads_table_el *ttable_get_element(struct nct_status_t *this, int index)
{
	struct threads_table_el *threads_table = this->threads_table;

	if (index < 0 || index >= this->threads_table_size) {
		nsi_print_error_and_exit("NCT fiber: thread index out of bounds (%i)\n", index);
	}

	while (index >= NCT_ALLOC_CHUNK_SIZE) {
		index -= NCT_ALLOC_CHUNK_SIZE;
		threads_table = threads_table[NCT_ALLOC_CHUNK_SIZE - 1].next;
	}

	return &threads_table[index];
}

static int ttable_get_empty_slot(struct nct_status_t *this)
{
	struct threads_table_el *tt_el = this->threads_table;

	for (int i = 0; i < this->threads_table_size; i++, tt_el = tt_el->next) {
		if (tt_el->state == NOTUSED ||
		    (NCT_REUSE_ABORTED_ENTRIES && tt_el->state == ABORTED)) {
			return i;
		}
	}

	struct threads_table_el *new_chunk = calloc(NCT_ALLOC_CHUNK_SIZE, sizeof(*new_chunk));

	if (new_chunk == NULL) {
		nsi_print_error_and_exit("NCT fiber: cannot grow thread table\n");
	}

	tt_el = ttable_get_element(this, this->threads_table_size - 1);
	tt_el->next = new_chunk;
	this->threads_table_size += NCT_ALLOC_CHUNK_SIZE;
	ttable_init_elements(new_chunk, NCT_ALLOC_CHUNK_SIZE);

	return this->threads_table_size - NCT_ALLOC_CHUNK_SIZE;
}

static void nct_context_init(struct threads_table_el *tt_el);

static void nct_fiber_start(void *arg)
{
	struct threads_table_el *tt_el = arg;
	struct nct_status_t *this = tt_el->nct_status;

	if (this->terminate || tt_el->state != USED) {
		nsi_print_error_and_exit("NCT fiber: entered a terminated thread\n");
	}

	tt_el->running = true;
	this->fptr(tt_el->payload);
	tt_el->running = false;
	tt_el->state = FAILED;
	nsi_print_error_and_exit("NCT fiber: hosted thread %i returned unexpectedly\n",
				 tt_el->thread_idx);
}

static void nct_context_init(struct threads_table_el *tt_el)
{
	long page_size_l = sysconf(_SC_PAGESIZE);

	if (page_size_l <= 0) {
		nsi_print_error_and_exit("NCT fiber: cannot query host page size\n");
	}

	size_t page_size = (size_t)page_size_l;
	size_t stack_size = (NSI_NCT_FIBER_STACK_SIZE + page_size - 1U) & ~(page_size - 1U);
	size_t mapping_size = stack_size + page_size;
	void *mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
			     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (mapping == MAP_FAILED) {
		nsi_print_error_and_exit("NCT fiber: stack mmap failed: %s\n", strerror(errno));
	}

	if (mprotect(mapping, page_size, PROT_NONE) != 0) {
		nsi_print_error_and_exit("NCT fiber: stack guard mprotect failed: %s\n",
				 strerror(errno));
	}

	tt_el->stack_mapping = mapping;
	tt_el->stack_mapping_size = mapping_size;
	tt_el->stack_addr = (char *)mapping + page_size;
	tt_el->stack_size = stack_size;

	memset(&tt_el->context, 0, sizeof(tt_el->context));
	uintptr_t stack_top = ((uintptr_t)tt_el->stack_addr + stack_size) & ~(uintptr_t)0xf;

	tt_el->context.rsp = stack_top;
	tt_el->context.rip = (uintptr_t)nct_context_bootstrap;
	tt_el->context.r12 = (uintptr_t)tt_el;
	tt_el->context.r13 = (uintptr_t)nct_fiber_start;
	__asm__ volatile("stmxcsr %0" : "=m"(tt_el->context.mxcsr));
	__asm__ volatile("fnstcw %0" : "=m"(tt_el->context.x87_cw));
}

void nct_swap_threads(void *this_arg, int next_allowed_thread_nbr)
{
	struct nct_status_t *this = this_arg;
	int current_idx = this->currently_allowed_thread;

	if (current_idx == -1) {
		nct_first_thread_start(this_arg, next_allowed_thread_nbr);
		__builtin_unreachable();
	}

	struct threads_table_el *current = ttable_get_element(this, current_idx);
	struct threads_table_el *next = ttable_get_element(this, next_allowed_thread_nbr);

	if (next->state != USED) {
		nsi_print_error_and_exit("NCT fiber: switching to non-runnable thread %i\n",
				 next_allowed_thread_nbr);
	}

	if (current_idx == next_allowed_thread_nbr) {
		return;
	}

	current->running = false;
	if (current->state == ABORTING) {
		current->state = ABORTED;
	}

	current->saved_errno = errno;
	errno = next->saved_errno;
	this->currently_allowed_thread = next_allowed_thread_nbr;
	next->running = true;

	nct_context_switch(&current->context, &next->context);

	/* The context resumed here after some later switch selected it again. */
	current->running = true;
}

void nct_first_thread_start(void *this_arg, int next_allowed_thread_nbr)
{
	struct nct_status_t *this = this_arg;
	struct threads_table_el *next = ttable_get_element(this, next_allowed_thread_nbr);

	if (next->state != USED) {
		nsi_print_error_and_exit("NCT fiber: first thread is not runnable\n");
	}

	this->root_errno = errno;
	errno = next->saved_errno;
	this->currently_allowed_thread = next_allowed_thread_nbr;
	next->running = true;
	nct_context_switch(&this->root_context, &next->context);

	nsi_print_error_and_exit("NCT fiber: returned to abandoned root context\n");
}

int nct_new_thread(void *this_arg, void *payload)
{
	struct nct_status_t *this = this_arg;
	int slot = ttable_get_empty_slot(this);
	struct threads_table_el *tt_el = ttable_get_element(this, slot);

	memset(tt_el->name, 0, sizeof(tt_el->name));
	tt_el->state = USED;
	tt_el->running = false;
	tt_el->thread_cnt = this->thread_create_count++;
	tt_el->payload = payload;
	tt_el->nct_status = this;
	tt_el->thread_idx = slot;
	tt_el->saved_errno = 0;
	nct_context_init(tt_el);

	return slot;
}

void nct_get_thread_stack(void *this_arg, int thread_idx, void **stack_addr,
			 unsigned long *stack_size)
{
	struct nct_status_t *this = this_arg;
	struct threads_table_el *tt_el = ttable_get_element(this, thread_idx);

	*stack_addr = tt_el->stack_addr;
	*stack_size = tt_el->stack_size;
}

void *nct_init(void (*fptr)(void *))
{
	struct nct_status_t *this = calloc(1, sizeof(*this));

	if (this == NULL) {
		nsi_print_error_and_exit("NCT fiber: cannot allocate state\n");
	}

	this->fptr = fptr;
	this->currently_allowed_thread = -1;
	this->threads_table = calloc(NCT_ALLOC_CHUNK_SIZE, sizeof(*this->threads_table));
	if (this->threads_table == NULL) {
		nsi_print_error_and_exit("NCT fiber: cannot allocate thread table\n");
	}
	this->threads_table_size = NCT_ALLOC_CHUNK_SIZE;
	ttable_init_elements(this->threads_table, NCT_ALLOC_CHUNK_SIZE);

	return this;
}

void nct_clean_up(void *this_arg)
{
	struct nct_status_t *this = this_arg;

	if (this != NULL) {
		this->terminate = true;
	}
	/*
	 * Match the pthread backend's conservative cleanup behavior. Context stacks
	 * remain mapped until process exit so cleanup can never unmap a live stack.
	 */
}

void nct_abort_thread(void *this_arg, int thread_idx)
{
	struct nct_status_t *this = this_arg;
	struct threads_table_el *tt_el = ttable_get_element(this, thread_idx);

	if (tt_el->state != USED && tt_el->state != ABORTING) {
		return;
	}

	if (thread_idx == this->currently_allowed_thread) {
		tt_el->state = ABORTING;
	} else {
		tt_el->state = ABORTED;
		tt_el->running = false;
	}
}

int nct_get_unique_thread_id(void *this_arg, int thread_idx)
{
	struct nct_status_t *this = this_arg;
	return ttable_get_element(this, thread_idx)->thread_cnt;
}

int nct_thread_name_set(void *this_arg, int thread_idx, const char *str)
{
	struct nct_status_t *this = this_arg;
	struct threads_table_el *tt_el = ttable_get_element(this, thread_idx);

	if (str == NULL) {
		return EINVAL;
	}

	strncpy(tt_el->name, str, sizeof(tt_el->name) - 1U);
	tt_el->name[sizeof(tt_el->name) - 1U] = '\0';
	return 0;
}
