/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NSI_COMMON_CORO_NSI_CORO_CTX_H
#define NSI_COMMON_CORO_NSI_CORO_CTX_H

#include <fenv.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef NSI_CORO_FIBERS
#include <stdint.h>
#include "nsi_fiber_ctx.h"
#else
#include "nsi_coro.h"

#if defined(__SANITIZE_ADDRESS__)
#define NSI_CORO_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NSI_CORO_ASAN 1
#endif
#endif

struct cxa_eh_globals {
	void *caught_exceptions;
	unsigned int uncaught_exceptions;
};

#ifdef _WIN32
#include <stdint.h>

/* The UCRT fenv_t keeps only abstract control and status bits. */
struct nsi_coro_fpenv {
	uint8_t x87[28];
	uint32_t mxcsr;
};
#endif

struct nsi_coro_ctx {
	struct cxa_eh_globals eh;
	nsi_coro_t continuation;
	void *map;
	size_t map_size;
	int err;
#ifdef _WIN32
	struct nsi_coro_fpenv fenv;
#else
	fenv_t fenv;
#endif
	void (*entry)(void *arg);
	void *arg;
#ifdef NSI_CORO_ASAN
	void *asan_fake_stack;
	const void *asan_bottom;
	size_t asan_size;
#endif
};
#endif /* NSI_CORO_FIBERS */

void nsi_coro_ctx_reset(void);
void nsi_coro_ctx_create(struct nsi_coro_ctx *ctx, void (*entry)(void *), void *arg);
void nsi_coro_ctx_free(struct nsi_coro_ctx *ctx);
void nsi_coro_ctx_stack(struct nsi_coro_ctx *ctx, void **addr, unsigned long *size);
void nsi_coro_ctx_switch(struct nsi_coro_ctx *to, bool final);
struct nsi_coro_ctx *nsi_coro_ctx_current(void);
void nsi_coro_ctx_defer_free(struct nsi_coro_ctx *ctx, void *allocation);

#endif /* NSI_COMMON_CORO_NSI_CORO_CTX_H */
