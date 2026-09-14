/* SPDX-License-Identifier: Apache-2.0 */
#include "nsi_host_time.h"

int nsi_host_clock_gettime(clockid_t clock_id, struct timespec *value)
{
	return clock_gettime(clock_id, value);
}

int nsi_host_nanosleep(const struct timespec *request, struct timespec *remaining)
{
	return nanosleep(request, remaining);
}
