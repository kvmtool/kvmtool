#ifndef KVM__MUTEX_H
#define KVM__MUTEX_H

#include <pthread.h>

#include "kvm/util.h"

/*
 * Kernel-alike mutex API - to make it easier for kernel developers
 * to write user-space code! :-)
 */

#define DEFINE_MUTEX(mutex) pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER

static inline void mutex_init(pthread_mutex_t *mutex)
{
	if (pthread_mutex_init(mutex, NULL) != 0)
		die("unexpected pthread_mutex_init() failure!");
}

static inline void mutex_lock(pthread_mutex_t *mutex)
{
	if (pthread_mutex_lock(mutex) != 0)
		die("unexpected pthread_mutex_lock() failure!");
}

static inline void mutex_unlock(pthread_mutex_t *mutex)
{
	if (pthread_mutex_unlock(mutex) != 0)
		die("unexpected pthread_mutex_unlock() failure!");
}

#endif /* KVM__MUTEX_H */
