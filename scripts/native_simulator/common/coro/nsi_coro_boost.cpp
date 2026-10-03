/*
 * Copyright (c) 2026 Quercus
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <boost/context/detail/fcontext.hpp>
#include <cstdint>
#include <cstdlib>
#include "nsi_coro.h"

using boost::context::detail::fcontext_t;
using boost::context::detail::jump_fcontext;
using boost::context::detail::make_fcontext;
using boost::context::detail::transfer_t;

struct continuation {
	fcontext_t context;
	nsi_coro_entry_f entry;
};

struct transfer_payload {
	continuation from;
	continuation *to;
};

static void trampoline(transfer_t transfer)
{
	auto *payload = static_cast<transfer_payload *>(transfer.data);

	payload->from.context = transfer.fctx;
	payload->to->entry(&payload->from);
	std::abort();
}

extern "C" nsi_coro_t nsi_coro_create(void *stack_top, nsi_coro_entry_f entry)
{
	auto *continuation = reinterpret_cast<struct continuation *>(
		static_cast<char *>(stack_top) - NSI_CORO_STACK_SIZE);
	uintptr_t stack_bottom = (reinterpret_cast<uintptr_t>(continuation + 1) + 15U) &
				 ~(uintptr_t)15U;

	continuation->entry = entry;
	continuation->context = make_fcontext(stack_top,
		reinterpret_cast<uintptr_t>(stack_top) - stack_bottom, trampoline);
	return continuation;
}

extern "C" nsi_coro_t nsi_coro_switch(nsi_coro_t target)
{
	transfer_payload payload = {};

	payload.to = static_cast<continuation *>(target);
	transfer_t transfer = jump_fcontext(payload.to->context, &payload);
	auto *incoming = static_cast<transfer_payload *>(transfer.data);

	incoming->from.context = transfer.fctx;
	return &incoming->from;
}
