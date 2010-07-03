#ifndef KVM__CPUFEATURE_H
#define KVM__CPUFEATURE_H

/*
 * CPUID flags we need to deal with
 */
#define KVM__X86_FEATURE_VMX		5	/* Hardware virtualization */
#define KVM__X86_FEATURE_XSAVE		26	/* XSAVE/XRSTOR/XSETBV/XGETBV */

#define cpu_feature_disable(reg, feature)	\
	((reg) & ~(1 << (feature)))
#define cpu_feature_enable(reg, feature)	\
	((reg) |  (1 << (feature)))

#endif /* KVM__CPUFEATURE_H */
