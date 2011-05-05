#ifndef KVM__THREADPOOL_H
#define KVM__THREADPOOL_H

struct kvm;

typedef void (*kvm_thread_callback_fn_t)(struct kvm *kvm, void *data);

int thread_pool__init(unsigned long thread_count);

void *thread_pool__add_job(struct kvm *kvm, kvm_thread_callback_fn_t callback, void *data);

void thread_pool__do_job(void *job);

#endif
