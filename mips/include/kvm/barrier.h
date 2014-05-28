#ifndef _KVM_BARRIER_H_
#define _KVM_BARRIER_H_

#define barrier() asm volatile("": : :"memory")

#define mb()	asm volatile (".set push\n\t.set mips2\n\tsync\n\t.set pop": : :"memory")
#define rmb() mb()
#define wmb() mb()

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
