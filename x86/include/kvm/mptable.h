#ifndef KVM_MPTABLE_H_
#define KVM_MPTABLE_H_

struct kvm;

int mptable__init(struct kvm *kvm);
int mptable__exit(struct kvm *kvm);

#endif /* KVM_MPTABLE_H_ */
