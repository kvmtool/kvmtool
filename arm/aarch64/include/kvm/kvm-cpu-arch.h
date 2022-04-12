#ifndef KVM__KVM_CPU_ARCH_H
#define KVM__KVM_CPU_ARCH_H

#include "kvm/kvm.h"

#include "arm-common/kvm-cpu-arch.h"

#define ARM_MPIDR_HWID_BITMASK	0xFF00FFFFFFUL
#define ARM_CPU_ID		3, 0, 0, 0
#define ARM_CPU_ID_MPIDR	5
#define ARM_CPU_CTRL		3, 0, 1, 0
#define ARM_CPU_CTRL_SCTLR_EL1	0

void kvm_cpu__select_features(struct kvm *kvm, struct kvm_vcpu_init *init);
int kvm_cpu__configure_features(struct kvm_cpu *vcpu);
int kvm_cpu__setup_pvtime(struct kvm_cpu *vcpu);
int kvm_cpu__teardown_pvtime(struct kvm *kvm);

#endif /* KVM__KVM_CPU_ARCH_H */
