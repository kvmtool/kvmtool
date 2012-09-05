#ifndef KVM__KVM_CPU_H
#define KVM__KVM_CPU_H

#include "kvm/kvm-cpu-arch.h"
#include <stdbool.h>

int kvm_cpu__init(struct kvm *kvm);
int kvm_cpu__exit(struct kvm *kvm);
struct kvm_cpu *kvm_cpu__arch_init(struct kvm *kvm, unsigned long cpu_id);
void kvm_cpu__delete(struct kvm_cpu *vcpu);
void kvm_cpu__reset_vcpu(struct kvm_cpu *vcpu);
void kvm_cpu__setup_cpuid(struct kvm_cpu *vcpu);
void kvm_cpu__enable_singlestep(struct kvm_cpu *vcpu);
void kvm_cpu__run(struct kvm_cpu *vcpu);
void kvm_cpu__reboot(struct kvm *kvm);
int kvm_cpu__start(struct kvm_cpu *cpu);
bool kvm_cpu__handle_exit(struct kvm_cpu *vcpu);

int kvm_cpu__get_debug_fd(void);
void kvm_cpu__set_debug_fd(int fd);
void kvm_cpu__show_code(struct kvm_cpu *vcpu);
void kvm_cpu__show_registers(struct kvm_cpu *vcpu);
void kvm_cpu__show_page_tables(struct kvm_cpu *vcpu);
void kvm_cpu__arch_nmi(struct kvm_cpu *cpu);

#endif /* KVM__KVM_CPU_H */
