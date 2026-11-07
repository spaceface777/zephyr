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
 * Define NSI_NO_SIGNAL_HANDLERS when compiling the runner if the embedding
 * application owns process signal handling. This also disables Windows console
 * handler registration. The simulator still honors its configured stop time.
 *
 * These APIs should be used with care, and not be used when the native
 * simulator main() is built in.
 *
 * Check nsi_main.c for more information.
 */

void nsi_init(int argc, char *argv[]);
void nsi_exec_for(uint64_t us);

/*
 * Free the stacks of the embedded CPUs and threads of a simulation that will never run again,
 * without running any embedded or hardware model code: for an embedding application which
 * abandons the simulation, for example after a fault inside it. The caller must not run on one
 * of those stacks. Host thread (pthread) backends free nothing.
 */
void nsi_abandon(void);

#ifdef __cplusplus
}
#endif

#endif /* NSI_COMMON_SRC_INCL_NSI_MAIN_SEMIPUBLIC_H */
