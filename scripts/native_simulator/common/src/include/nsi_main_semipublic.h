/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NSI_COMMON_SRC_INCL_NSI_MAIN_SEMIPUBLIC_H
#define NSI_COMMON_SRC_INCL_NSI_MAIN_SEMIPUBLIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * These APIs are exposed for special use cases in which a developer needs to
 * replace the native simulator main loop.
 * An example of such a case is LLVMs fuzzing support. For this one sets
 * NSI_NO_MAIN, and provides an specialized main() or hooks into the tooling
 * provided main().
 *
 * These APIs should be used with care, and not be used when the native
 * simulator main() is built in.
 *
 * Check nsi_main.c for more information.
 */

void nsi_init(int argc, char *argv[]);
void nsi_exec_for(uint64_t us);

/*
 * nsi_init() is these two steps, followed by nothing else:
 * nsi_init_until_boot() initializes everything up to, but excluding, the CPU
 * boot. nsi_boot() then boots the CPUs and runs the FIRST_SLEEP tasks.
 * A program which drives the simulator itself can do work in between,
 * knowing no embedded code has run yet.
 */
void nsi_init_until_boot(int argc, char *argv[]);
void nsi_boot(void);

#ifdef __cplusplus
}
#endif

#endif /* NSI_COMMON_SRC_INCL_NSI_MAIN_SEMIPUBLIC_H */
