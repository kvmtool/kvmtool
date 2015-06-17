#ifndef _KVM_BARRIER_H_
#define _KVM_BARRIER_H_

#define mb()   asm volatile ("sync" : : : "memory")
#define rmb()  asm volatile ("sync" : : : "memory")
#define wmb()  asm volatile ("sync" : : : "memory")

#endif /* _KVM_BARRIER_H_ */
