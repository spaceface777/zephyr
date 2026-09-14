/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NSI_HOST_TIME_H
#define NSI_HOST_TIME_H

/* Runner-only host operations. This header must not enter embedded libc code. */
#include <time.h>

int nsi_host_clock_gettime(clockid_t clock_id, struct timespec *value);
int nsi_host_nanosleep(const struct timespec *request, struct timespec *remaining);

#endif
