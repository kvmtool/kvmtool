#ifndef KVM__MUTEX_H
#define KVM__MUTEX_H

#include <pthread.h>

#include "kvm/util.h"

/*
 * Kernel-alike mutex API - to make it easier for kernel developers
 * to write user-space code! :-)
 */

struct mutex {
	pthread_mutex_t mutex;
};
#define MUTEX_INITIALIZER (struct mutex) { .mutex = PTHREAD_MUTEX_INITIALIZER }

#define DEFINE_MUTEX(mtx) struct mutex mtx = MUTEX_INITIALIZER

static inline void mutex_init(struct mutex *lock)
{
	if (pthread_mutex_init(&lock->mutex, NULL) != 0)
		die("unexpected pthread_mutex_init() failure!");
}

static inline void mutex_lock(struct mutex *lock)
{
	if (pthread_mutex_lock(&lock->mutex) != 0)
		die("unexpected pthread_mutex_lock() failure!");

}

static inline void mutex_unlock(struct mutex *lock)
{
	if (pthread_mutex_unlock(&lock->mutex) != 0)
		die("unexpected pthread_mutex_unlock() failure!");
}

#endif /* KVM__MUTEX_H */
