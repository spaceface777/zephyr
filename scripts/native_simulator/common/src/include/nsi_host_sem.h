/*
 * Copyright (c) 2026 Zephyr Project
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef NSI_HOST_SEM_H
#define NSI_HOST_SEM_H

#include <errno.h>

#ifdef __APPLE__

#include <pthread.h>

struct nsi_sem {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	unsigned int value;
};

typedef struct nsi_sem nsi_sem_t;

static inline int nsi_sem_error(int error)
{
	if (error == 0) {
		return 0;
	}
	errno = error;
	return -1;
}

static inline int nsi_sem_init(nsi_sem_t *sem, unsigned int value)
{
	int error = pthread_mutex_init(&sem->mutex, NULL);

	if (error != 0) {
		return nsi_sem_error(error);
	}
	error = pthread_cond_init(&sem->condition, NULL);
	if (error != 0) {
		(void)pthread_mutex_destroy(&sem->mutex);
		return nsi_sem_error(error);
	}
	sem->value = value;
	return 0;
}

static inline int nsi_sem_wait(nsi_sem_t *sem)
{
	int error = pthread_mutex_lock(&sem->mutex);

	if (error != 0) {
		return nsi_sem_error(error);
	}
	while (sem->value == 0U) {
		error = pthread_cond_wait(&sem->condition, &sem->mutex);
		if (error != 0) {
			(void)pthread_mutex_unlock(&sem->mutex);
			return nsi_sem_error(error);
		}
	}
	sem->value--;
	error = pthread_mutex_unlock(&sem->mutex);
	return nsi_sem_error(error);
}

static inline int nsi_sem_post(nsi_sem_t *sem)
{
	int error = pthread_mutex_lock(&sem->mutex);

	if (error != 0) {
		return nsi_sem_error(error);
	}
	sem->value++;
	error = pthread_cond_signal(&sem->condition);
	if (error == 0) {
		error = pthread_mutex_unlock(&sem->mutex);
	} else {
		(void)pthread_mutex_unlock(&sem->mutex);
	}
	return nsi_sem_error(error);
}

#else

#include <semaphore.h>

typedef sem_t nsi_sem_t;

static inline int nsi_sem_init(nsi_sem_t *sem, unsigned int value)
{
	return sem_init(sem, 0, value);
}

static inline int nsi_sem_wait(nsi_sem_t *sem)
{
	return sem_wait(sem);
}

static inline int nsi_sem_post(nsi_sem_t *sem)
{
	return sem_post(sem);
}

#endif

#endif /* NSI_HOST_SEM_H */
