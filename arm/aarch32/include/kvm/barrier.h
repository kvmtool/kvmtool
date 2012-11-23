#ifndef KVM__KVM_BARRIER_H
#define KVM__KVM_BARRIER_H

#define dmb()	asm volatile ("dmb" : : : "memory")

#define mb()	dmb()
#define rmb()	dmb()
#define wmb()	dmb()

#endif /* KVM__KVM_BARRIER_H */
