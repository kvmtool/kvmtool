#ifndef KVM__RWSEM_H
#define KVM__RWSEM_H

#include <pthread.h>

#include "kvm/util.h"

/*
 * Kernel-alike rwsem API - to make it easier for kernel developers
 * to write user-space code! :-)
 */

#define DECLARE_RWSEM(sem) pthread_rwlock_t sem = PTHREAD_RWLOCK_INITIALIZER

static inline void down_read(pthread_rwlock_t *rwsem)
{
	if (pthread_rwlock_rdlock(rwsem) != 0)
		die("unexpected pthread_rwlock_rdlock() failure!");
}

static inline void down_write(pthread_rwlock_t *rwsem)
{
	if (pthread_rwlock_wrlock(rwsem) != 0)
		die("unexpected pthread_rwlock_wrlock() failure!");
}

static inline void up_read(pthread_rwlock_t *rwsem)
{
	if (pthread_rwlock_unlock(rwsem) != 0)
		die("unexpected pthread_rwlock_unlock() failure!");
}

static inline void up_write(pthread_rwlock_t *rwsem)
{
	if (pthread_rwlock_unlock(rwsem) != 0)
		die("unexpected pthread_rwlock_unlock() failure!");
}

#endif /* KVM__RWSEM_H */
