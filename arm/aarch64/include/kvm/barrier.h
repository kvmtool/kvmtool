#ifndef KVM__KVM_BARRIER_H
#define KVM__KVM_BARRIER_H

#define mb()	asm volatile ("dmb ish"		: : : "memory")
#define rmb()	asm volatile ("dmb ishld"	: : : "memory")
#define wmb()	asm volatile ("dmb ishst"	: : : "memory")

#endif /* KVM__KVM_BARRIER_H */
