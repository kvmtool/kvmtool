#ifndef KVM__BRLOCK_H
#define KVM__BRLOCK_H

#include "kvm/kvm.h"
#include "kvm/barrier.h"

/*
 * brlock is a lock which is very cheap for reads, but very expensive
 * for writes.
 * This lock will be used when updates are very rare and reads are common.
 * This lock is currently implemented by stopping the guest while
 * performing the updates. We assume that the only threads whichread from
 * the locked data are VCPU threads, and the only writer isn't a VCPU thread.
 */

#ifndef barrier
#define barrier()		__asm__ __volatile__("": : :"memory")
#endif

#ifdef KVM_BRLOCK_DEBUG

#include "kvm/rwsem.h"

DECLARE_RWSEM(brlock_sem);

#define br_read_lock(kvm)	down_read(&brlock_sem);
#define br_read_unlock(kvm)	up_read(&brlock_sem);

#define br_write_lock(kvm)	down_write(&brlock_sem);
#define br_write_unlock(kvm)	up_write(&brlock_sem);

#else

#define br_read_lock(kvm)	barrier()
#define br_read_unlock(kvm)	barrier()

#define br_write_lock(kvm)	kvm__pause(kvm)
#define br_write_unlock(kvm)	kvm__continue(kvm)
#endif

#endif
