#ifndef _KVM_BARRIER_H_
#define _KVM_BARRIER_H_

#define barrier() asm volatile("": : :"memory")

#define mb()	asm volatile ("mfence": : :"memory")
#define rmb()	asm volatile ("lfence": : :"memory")
#define wmb()	asm volatile ("sfence": : :"memory")

#ifdef CONFIG_SMP
#define smp_mb()	mb()
#define smp_rmb()	rmb()
#define smp_wmb()	wmb()
#else
#define smp_mb()	barrier()
#define smp_rmb()	barrier()
#define smp_wmb()	barrier()
#endif

#endif /* _KVM_BARRIER_H_ */
