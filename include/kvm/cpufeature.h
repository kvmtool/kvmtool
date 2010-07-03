#ifndef KVM__CPUFEATURE_H
#define KVM__CPUFEATURE_H

#define CPUID_VENDOR_INTEL_1 0x756e6547 /* "Genu" */
#define CPUID_VENDOR_INTEL_2 0x49656e69 /* "ineI" */
#define CPUID_VENDOR_INTEL_3 0x6c65746e /* "ntel" */

#define CPUID_VENDOR_AMD_1   0x68747541 /* "Auth" */
#define CPUID_VENDOR_AMD_2   0x69746e65 /* "enti" */
#define CPUID_VENDOR_AMD_3   0x444d4163 /* "cAMD" */

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
