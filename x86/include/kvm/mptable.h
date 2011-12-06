#ifndef KVM_MPTABLE_H_
#define KVM_MPTABLE_H_

struct kvm;

void mptable_setup(struct kvm *kvm, unsigned int ncpus);

#endif /* KVM_MPTABLE_H_ */
