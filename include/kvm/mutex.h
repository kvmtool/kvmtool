#ifndef KVM__MUTEX_H
#define KVM__MUTEX_H

#include <pthread.h>

#include "kvm/util.h"

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
